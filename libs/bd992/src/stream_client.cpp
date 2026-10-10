// SPDX-License-Identifier: GPL-3.0-or-later

#include "bd992/stream_client.h"

#include "byte_stream/backoff.h"

#include <array>

#include <spdlog/spdlog.h>

namespace bd992
{

StreamClient::StreamClient(StreamFactory factory, Options options, RecordHandler onRecord) :
    mFactory(std::move(factory)),
    mOptions(std::move(options)),
    mOnRecord(std::move(onRecord))
{
    if (mOptions.reconnectBackoff.empty())
    {
        mOptions.reconnectBackoff.push_back(std::chrono::milliseconds(1000));
    }
}

StreamClient::~StreamClient()
{
    stop();
}

void StreamClient::setTransmissionHandler(TransmissionHandler handler)
{
    mOnTransmission = std::move(handler);
}

void StreamClient::setByteTap(ByteTap tap)
{
    mByteTap = std::move(tap);
}

void StreamClient::start()
{
    if (mRunning.load())
    {
        return;
    }

    mStopping.store(false);
    mRunning.store(true);
    mThread = std::thread([this] { run(); });
}

void StreamClient::stop()
{
    mStopping.store(true);
    if (mThread.joinable())
    {
        mThread.join();
    }
    mRunning.store(false);
}

StreamClient::Stats StreamClient::stats() const
{
    const std::lock_guard<std::mutex> lock(mStatsMutex);
    Stats copy = mStats;
    copy.framer = mFramer.stats();
    copy.assembler = mAssembler.stats();
    return copy;
}

void StreamClient::deliverTransmission(std::span<const std::uint8_t> payload)
{
    gsof::RecordIterator it(payload);

    while (!it.done())
    {
        const gsof::Result<gsof::RawRecord> record = it.next();
        if (!record.has_value())
        {
            // A length byte that cannot be trusted. The walk has already
            // stopped; count it and move on to the next transmission.
            const std::lock_guard<std::mutex> lock(mStatsMutex);
            ++mStats.malformedRecords;
            break;
        }

        {
            const std::lock_guard<std::mutex> lock(mStatsMutex);
            ++mStats.records;
            if (!gsof::is_known_record(record->type))
            {
                ++mStats.unknownRecords;
            }
        }

        // Delivered whether or not we model it: the node decides what to do
        // with an unknown record, and passing the bytes through is one of the
        // options.
        if (mOnRecord)
        {
            mOnRecord(*record);
        }
    }

    if (mOnTransmission)
    {
        mOnTransmission();
    }
}

void StreamClient::consume(std::span<const std::uint8_t> bytes)
{
    if (mByteTap)
    {
        mByteTap(bytes);
    }

    mFramer.push(bytes);

    while (const auto packet = mFramer.next())
    {
        if (!packet->is(gsof::trimcomm::PacketType::GenOut))
        {
            // The stream socket carries GSOF and nothing else. Anything else
            // here is a misconfigured port -- a command reply arriving on the
            // data connection -- and is dropped rather than fed to the page
            // assembler, whose transport header this packet may not have.
            SPDLOG_DEBUG("bd992: ignoring packet type 0x{:02x} on the GSOF stream", packet->type);
            continue;
        }

        const gsof::Result<gsof::PageAssembler::Feed> fed = mAssembler.feed(packet->data);
        if (!fed.has_value())
        {
            SPDLOG_DEBUG("bd992: page rejected: {}", gsof::to_string(fed.error().kind));
            continue;
        }

        if (*fed == gsof::PageAssembler::Feed::Complete)
        {
            {
                const std::lock_guard<std::mutex> lock(mStatsMutex);
                ++mStats.transmissions;
            }
            deliverTransmission(mAssembler.payload());
        }
    }
}

void StreamClient::run()
{
    byte_stream::Backoff backoff(mOptions.reconnectBackoff);

    while (!mStopping.load())
    {
        Result<std::unique_ptr<byte_stream::ByteStream>> stream = mFactory();

        if (!stream.has_value() || *stream == nullptr)
        {
            {
                const std::lock_guard<std::mutex> lock(mStatsMutex);
                ++mStats.connectFailures;
                mStats.connected = false;
                mStats.lastError = stream.has_value() ? "no stream" : to_string(stream.error());
            }

            if (mOptions.stopWhenStreamEnds)
            {
                break;
            }

            byte_stream::sleepWhile(backoff.failed(std::chrono::steady_clock::now()),
                                    [this] { return !mStopping.load(); });
            continue;
        }

        backoff.connected(std::chrono::steady_clock::now());

        // Anything buffered came from the previous connection and cannot be
        // part of a packet that arrives on this one.
        mFramer.reset();
        mAssembler.reset();

        {
            const std::lock_guard<std::mutex> lock(mStatsMutex);
            ++mStats.connects;
            mStats.connected = true;
            mStats.lastError.clear();
        }

        std::array<std::uint8_t, 8192> buffer {};

        while (!mStopping.load())
        {
            const ssize_t n = (*stream)->recvSome(buffer, static_cast<unsigned>(mOptions.readTimeout.count()));

            if (n < 0)
            {
                break;
            }
            if (n == 0)
            {
                // Timed out with nothing available. Normal: a 1 Hz record
                // means four of these between messages.
                continue;
            }

            {
                const std::lock_guard<std::mutex> lock(mStatsMutex);
                mStats.bytesRead += static_cast<std::uint64_t>(n);
            }

            consume(std::span<const std::uint8_t>(buffer.data(), static_cast<std::size_t>(n)));
        }

        {
            const std::lock_guard<std::mutex> lock(mStatsMutex);
            mStats.connected = false;
            ++mStats.disconnects;
        }

        (*stream)->close();

        if (mOptions.stopWhenStreamEnds)
        {
            break;
        }

        // A dropped connection waits too. Going straight back to the factory
        // turned a receiver that accepts and then closes into a tight loop.
        byte_stream::sleepWhile(backoff.failed(std::chrono::steady_clock::now()),
                                    [this] { return !mStopping.load(); });
    }

    mRunning.store(false);
}

} // namespace bd992
