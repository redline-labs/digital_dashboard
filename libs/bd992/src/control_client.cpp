// SPDX-License-Identifier: GPL-3.0-or-later

#include "bd992/control_client.h"

#include <array>
#include <optional>

#include <spdlog/spdlog.h>

#include "gsof/transport.h"

namespace bd992
{

namespace
{

// Long enough that a poll is cheap, short enough that a reply is not delayed
// by a whole slice after it arrives.
constexpr unsigned kReadSliceMs = 50;

} // namespace

ControlClient::ControlClient(StreamFactory factory, Options options) :
    mFactory(std::move(factory)),
    mOptions(options)
{
}

ControlClient::~ControlClient()
{
    disconnect();
}

std::uint8_t ControlClient::deviceType() const
{
    const std::lock_guard<std::mutex> lock(mMutex);
    return mDeviceType;
}

void ControlClient::disconnect()
{
    const std::lock_guard<std::mutex> lock(mMutex);
    if (mStream)
    {
        mStream->close();
        mStream.reset();
    }
    mFramer.reset();
}

Result<ByteStream*> ControlClient::ensureConnected()
{
    if (mStream && mStream->isOpen())
    {
        return mStream.get();
    }

    mStream.reset();
    mFramer.reset();

    Result<std::unique_ptr<ByteStream>> opened = mFactory();
    if (!opened.has_value())
    {
        return std::unexpected(opened.error());
    }
    if (*opened == nullptr)
    {
        return not_connected("control connection unavailable");
    }

    mStream = std::move(*opened);
    return mStream.get();
}

Result<void> ControlClient::readUntil(std::chrono::steady_clock::time_point deadline,
                                      const std::function<bool(const gsof::trimcomm::PacketView&)>& accept)
{
    std::array<std::uint8_t, 2048> buffer {};

    while (std::chrono::steady_clock::now() < deadline)
    {
        // Anything already buffered from the previous read may hold the whole
        // reply, so drain before reading again.
        while (const auto packet = mFramer.next())
        {
            if (accept(*packet))
            {
                return {};
            }
        }

        const ssize_t n = mStream->recvSome(buffer, kReadSliceMs);
        if (n < 0)
        {
            mStream->close();
            return io_error("control connection closed while waiting for a reply");
        }
        if (n == 0)
        {
            continue;
        }

        mFramer.push(std::span<const std::uint8_t>(buffer.data(), static_cast<std::size_t>(n)));
    }

    // One last drain: the deadline may have passed between the read and the
    // loop condition.
    while (const auto packet = mFramer.next())
    {
        if (accept(*packet))
        {
            return {};
        }
    }

    return timeout("no reply from the receiver");
}

Result<ControlClient::Reply> ControlClient::exchangeLocked(std::span<const std::uint8_t> packet)
{
    const Result<ByteStream*> stream = ensureConnected();
    if (!stream.has_value())
    {
        return std::unexpected(stream.error());
    }

    if (!(*stream)->sendAll(packet))
    {
        (*stream)->close();
        return io_error("failed to send command to the receiver");
    }

    std::optional<Reply> reply;
    bool wasNak = false;

    const Result<void> read = readUntil(
        std::chrono::steady_clock::now() + mOptions.replyTimeout,
        [&reply, &wasNak](const gsof::trimcomm::PacketView& view) {
            if (view.is(gsof::trimcomm::PacketType::GenOut))
            {
                // A GSOF report on the control port. Harmless -- it means the
                // same physical port is also configured for output -- and
                // certainly not the answer to this question.
                return false;
            }

            if (view.is(gsof::trimcomm::PacketType::Nak))
            {
                wasNak = true;
                return true;
            }

            reply = Reply { view.status, view.type,
                            std::vector<std::uint8_t>(view.data.begin(), view.data.end()) };
            return true;
        });

    if (!read.has_value())
    {
        return std::unexpected(read.error());
    }

    if (wasNak)
    {
        // The receiver understood and declined. Resending the same bytes will
        // get the same answer, so this is not a retryable error.
        return refused("the receiver rejected the command");
    }

    if (!reply.has_value())
    {
        return protocol_error("no usable reply");
    }

    return *reply;
}

Result<ControlClient::Reply> ControlClient::exchange(std::span<const std::uint8_t> packet)
{
    const std::lock_guard<std::mutex> lock(mMutex);
    return exchangeLocked(packet);
}

Result<gsof::appfile::ApplicationFile> ControlClient::readApplicationFile()
{
    return readApplicationFile(mOptions.applicationFileIndex);
}

Result<gsof::appfile::ApplicationFile> ControlClient::readApplicationFile(std::uint16_t index)
{
    const std::lock_guard<std::mutex> lock(mMutex);

    const Result<ByteStream*> stream = ensureConnected();
    if (!stream.has_value())
    {
        return std::unexpected(stream.error());
    }

    const auto request = gsof::appfile::get_application_file(index);
    if (!(*stream)->sendAll(request))
    {
        (*stream)->close();
        return io_error("failed to request the application file");
    }

    // The reply is an APPFILE, which may span several pages. It carries the
    // same three-byte transport header as a GSOF report, so the same assembler
    // puts it back together -- that shared header is why there is one
    // reassembly implementation rather than two.
    gsof::PageAssembler assembler;
    bool complete = false;
    bool refusedByReceiver = false;

    const Result<void> read = readUntil(
        std::chrono::steady_clock::now() + mOptions.replyTimeout,
        [&assembler, &complete, &refusedByReceiver](const gsof::trimcomm::PacketView& view) {
            if (view.is(gsof::trimcomm::PacketType::Nak))
            {
                refusedByReceiver = true;
                return true;
            }
            if (!view.is(gsof::trimcomm::PacketType::AppFile))
            {
                return false;
            }

            const gsof::Result<gsof::PageAssembler::Feed> fed = assembler.feed(view.data);
            if (!fed.has_value())
            {
                SPDLOG_WARN("bd992: application file page rejected: {}", gsof::to_string(fed.error().kind));
                return false;
            }

            complete = *fed == gsof::PageAssembler::Feed::Complete;
            return complete;
        });

    if (!read.has_value())
    {
        return std::unexpected(read.error());
    }
    if (refusedByReceiver)
    {
        return refused("the receiver has no application file at index " + std::to_string(index));
    }
    if (!complete)
    {
        return protocol_error("the application file did not arrive complete");
    }

    const gsof::Result<gsof::appfile::ApplicationFile> file =
        gsof::appfile::parse_application_file(assembler.payload());
    if (!file.has_value())
    {
        return protocol_error(std::string("cannot decode the application file: ") +
                              gsof::to_string(file.error().kind));
    }

    // Remember what the receiver calls itself, so a subsequent write echoes it
    // rather than guessing. The ICD publishes no device type values.
    mDeviceType = file->control.deviceType;

    return *file;
}

Result<void> ControlClient::writeApplicationFile(std::span<const std::uint8_t> records)
{
    if (records.empty())
    {
        return invalid_argument("refusing to send an application file with no records");
    }

    const std::lock_guard<std::mutex> lock(mMutex);

    gsof::appfile::FileControl control {};
    control.deviceType = mDeviceType;
    control.startImmediately = true;
    control.resetToFactoryFirst = false;

    std::array<std::uint8_t, gsof::trimcomm::kMaxPacketSize> packet {};
    const gsof::Result<std::size_t> written =
        gsof::appfile::encode_application_file(control, records, mTransmissionNumber, packet);

    if (!written.has_value())
    {
        return invalid_argument(std::string("cannot encode the application file: ") +
                                gsof::to_string(written.error().kind));
    }

    ++mTransmissionNumber;

    const Result<Reply> reply =
        exchangeLocked(std::span<const std::uint8_t>(packet.data(), *written));
    if (!reply.has_value())
    {
        return std::unexpected(reply.error());
    }

    if (reply->type != static_cast<std::uint8_t>(gsof::trimcomm::PacketType::Ack))
    {
        return protocol_error("the receiver answered an application file with packet type 0x" +
                              std::to_string(reply->type) + " rather than ACK");
    }

    return {};
}

Result<ControlClient::Reply> ControlClient::readOptions(std::uint8_t page)
{
    const std::lock_guard<std::mutex> lock(mMutex);

    const auto request = gsof::appfile::get_options(page);
    const Result<Reply> reply = exchangeLocked(request);
    if (!reply.has_value())
    {
        return std::unexpected(reply.error());
    }

    if (reply->type != static_cast<std::uint8_t>(gsof::trimcomm::PacketType::RetOpt))
    {
        return protocol_error("GETOPT was answered with an unexpected packet type");
    }

    return *reply;
}

Result<ControlClient::Reply> ControlClient::sendRaw(std::uint8_t packetType, std::span<const std::uint8_t> data)
{
    if (!mOptions.allowRawCommands)
    {
        return not_permitted("raw commands are disabled; set allow_raw_commands in the node config");
    }

    std::array<std::uint8_t, gsof::trimcomm::kMaxPacketSize> packet {};
    const gsof::Result<std::size_t> written = gsof::trimcomm::encode_packet(
        static_cast<gsof::trimcomm::PacketType>(packetType), data, packet);

    if (!written.has_value())
    {
        return invalid_argument("the raw command does not fit in one packet");
    }

    const std::lock_guard<std::mutex> lock(mMutex);
    return exchangeLocked(std::span<const std::uint8_t>(packet.data(), *written));
}

} // namespace bd992
