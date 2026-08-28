// SPDX-License-Identifier: GPL-3.0-or-later

#include "xpr/radio.h"

#include <algorithm>
#include <array>
#include <thread>
#include <utility>

#include <spdlog/spdlog.h>

namespace xpr
{

namespace
{

// Short names for the protocol library, and one alias that matters: a
// `Decoded<T>` is a mototrbo::Result -- a DECODE that succeeded or failed --
// while a bare `Result<T>` is this library's, about the connection. The two
// error types are deliberately different (see xpr/error.h) and mixing them up
// is how a refusal turns into "protocol error".
namespace xnl = mototrbo::xnl;
namespace xcmp = mototrbo::xcmp;
namespace control = mototrbo::control;

template <typename T>
using Decoded = mototrbo::Result<T>;

constexpr std::size_t kReadChunk = 512;

// A frame cannot exceed a 16-bit length plus its prefix. Anything past this in
// the reassembly buffer means the stream is not XNL at all -- a wrong port, or
// a socket that came back as something else -- and continuing to buffer it
// would grow without limit.
constexpr std::size_t kMaxBufferedBytes = 4 * (0xFFFFu + xnl::kFrameOverhead);

// The handshake's first step waits for a broadcast the radio sends on its own
// about 5 ms after accepting the connection. This is how long to wait before
// falling back to asking, which a healthy radio never needs.
constexpr std::chrono::milliseconds kMasterStatusWait { 750 };

// Every frame this library sends: a command of at most kMaxCommandSize, or the
// 12-byte handshake payload.
constexpr std::size_t kSendBufferSize = xnl::kFrameOverhead + 32;

bool isSessionFailure(Error::Kind kind)
{
    switch (kind)
    {
        case Error::Kind::NotFound:
        case Error::Kind::ConnectFailed:
        case Error::Kind::NotConnected:
        case Error::Kind::Io:
        case Error::Kind::Timeout:
        case Error::Kind::AuthFailed:
            return true;
        case Error::Kind::Refused:
        case Error::Kind::Protocol:
        case Error::Kind::InvalidArgument:
        case Error::Kind::NotPermitted:
            return false;
    }

    return false;
}

unsigned millisecondsUntil(std::chrono::steady_clock::time_point deadline)
{
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline)
    {
        return 0;
    }

    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    return static_cast<unsigned>(std::max<std::int64_t>(1, remaining));
}

} // namespace

Radio::Radio(StreamFactory factory, Options options) :
    mFactory(std::move(factory)),
    mOptions(std::move(options))
{
}

Radio::~Radio() = default;

bool Radio::connected() const
{
    const std::lock_guard<std::mutex> lock(mMutex);
    return mConnected;
}

Radio::Stats Radio::stats() const
{
    const std::lock_guard<std::mutex> lock(mMutex);
    Stats out = mStats;
    out.connected = mConnected;
    out.address = mAddress;
    return out;
}

Result<void> Radio::connect()
{
    const std::lock_guard<std::mutex> lock(mMutex);
    return ensureConnectedLocked(true);
}

void Radio::disconnect()
{
    const std::lock_guard<std::mutex> lock(mMutex);
    dropConnectionLocked("closed by request");
}

void Radio::dropConnectionLocked(std::string reason)
{
    if (mStream)
    {
        mStream->close();
        mStream.reset();
    }

    if (mConnected)
    {
        ++mStats.disconnects;
        SPDLOG_WARN("xpr: session lost: {}", reason);
    }

    mConnected = false;
    mAddress = 0;
    mMasterAddress = 0;
    mRxBuffer.clear();
    mStats.lastError = std::move(reason);
}

std::uint8_t Radio::nextFlagsLocked()
{
    mFlags = static_cast<std::uint8_t>((mFlags + 1) & 0x07u);
    return mFlags;
}

std::uint16_t Radio::nextTransactionLocked()
{
    // Rolling, and it skips zero: the radio echoes this back and a zero is
    // indistinguishable from a frame that carries no transaction at all.
    ++mTransaction;
    if (mTransaction == 0)
    {
        mTransaction = 1;
    }

    return mTransaction;
}

Result<void> Radio::ensureConnectedLocked(bool force)
{
    if (mConnected && mStream && mStream->isOpen())
    {
        return {};
    }

    const auto now = std::chrono::steady_clock::now();
    if (!force && now < mNextConnectAttempt)
    {
        return not_connected("waiting before the next connection attempt");
    }

    // Whatever state a half-open session left behind is not reusable.
    mStream.reset();
    mRxBuffer.clear();
    mConnected = false;
    mAddress = 0;
    mMasterAddress = 0;
    mFlags = 0;

    const auto scheduleRetry = [this, now] {
        ++mStats.connectFailures;
        if (mOptions.reconnectBackoff.empty())
        {
            mNextConnectAttempt = now;
            return;
        }

        const std::size_t index = std::min(mBackoffIndex, mOptions.reconnectBackoff.size() - 1);
        mNextConnectAttempt = now + mOptions.reconnectBackoff[index];
        if (mBackoffIndex + 1 < mOptions.reconnectBackoff.size())
        {
            ++mBackoffIndex;
        }
    };

    Result<std::unique_ptr<ByteStream>> stream = mFactory();
    if (!stream.has_value())
    {
        mStats.lastError = to_string(stream.error());
        scheduleRetry();
        return std::unexpected(stream.error());
    }

    mStream = std::move(*stream);

    if (const Result<void> shook = handshakeLocked(); !shook.has_value())
    {
        mStats.lastError = to_string(shook.error());
        mStream.reset();
        scheduleRetry();
        return shook;
    }

    ++mStats.connects;
    mBackoffIndex = 0;
    mConnected = true;
    mStats.lastError.clear();
    SPDLOG_INFO("xpr: session up, our address 0x{:04x}, master 0x{:04x}", mAddress, mMasterAddress);
    return {};
}

Result<void> Radio::handshakeLocked()
{
    const auto deadline = std::chrono::steady_clock::now() + mOptions.handshakeTimeout;

    // 1) Learn the master's address. The radio broadcasts it unprompted a few
    //    milliseconds after accepting the connection, so the query below is a
    //    fallback a healthy radio never needs.
    Result<xnl::Frame> master = readFrameOfLocked(
        xnl::Opcode::MasterStatusBroadcast,
        std::min(deadline, std::chrono::steady_clock::now() + kMasterStatusWait));

    if (!master.has_value())
    {
        if (const Result<void> sent = sendFrameLocked(xnl::Opcode::DeviceMasterQuery, 0, 0, 0, 0, 0, {});
            !sent.has_value())
        {
            return sent;
        }

        master = readFrameOfLocked(xnl::Opcode::MasterStatusBroadcast, deadline);
        if (!master.has_value())
        {
            return std::unexpected(master.error());
        }
    }

    mMasterAddress = master->src;

    // 2) Ask for the authentication challenge.
    if (const Result<void> sent =
            sendFrameLocked(xnl::Opcode::DeviceAuthRequest, 0, 0, mMasterAddress, 0, 0, {});
        !sent.has_value())
    {
        return sent;
    }

    const Result<xnl::Frame> authReply = readFrameOfLocked(xnl::Opcode::DeviceAuthReply, deadline);
    if (!authReply.has_value())
    {
        return std::unexpected(authReply.error());
    }

    const Decoded<xnl::AuthChallenge> challenge = xnl::parse_auth_reply(authReply->payload);
    if (!challenge.has_value())
    {
        return from_protocol(challenge.error(), "auth reply");
    }

    // 3) Answer it.
    //
    // The unacknowledged-delivery flag is not optional. Without it the radio
    // expects a DATA_MSG_ACK for every message it sends, retransmits each
    // reply five times when it does not get one, and every query then returns
    // the PREVIOUS query's answer -- a failure that looks like a decoding bug
    // and is not one.
    const std::array<std::uint8_t, xnl::kConnRequestPayloadSize> payload =
        xnl::conn_request_payload(challenge->challenge);

    if (const Result<void> sent =
            sendFrameLocked(xnl::Opcode::DeviceConnRequest, 0, xnl::kConnFlagUnacked, mMasterAddress,
                            challenge->temporaryAddress, nextTransactionLocked(), payload);
        !sent.has_value())
    {
        return sent;
    }

    const Result<xnl::Frame> connReply = readFrameOfLocked(xnl::Opcode::DeviceConnReply, deadline);
    if (!connReply.has_value())
    {
        return std::unexpected(connReply.error());
    }

    const Decoded<std::uint16_t> address = xnl::parse_conn_reply(connReply->payload);
    if (!address.has_value())
    {
        return from_protocol(address.error(), "conn reply");
    }

    mAddress = *address;
    return {};
}

Result<void> Radio::sendFrameLocked(xnl::Opcode opcode, std::uint8_t protocol, std::uint8_t flags,
                                    std::uint16_t dst, std::uint16_t src, std::uint16_t transaction,
                                    std::span<const std::uint8_t> payload)
{
    if (!mStream)
    {
        return not_connected("no stream");
    }

    xnl::Frame frame;
    frame.opcode = opcode;
    frame.protocol = protocol;
    frame.flags = flags;
    frame.dst = dst;
    frame.src = src;
    frame.transaction = transaction;
    frame.payload = payload;

    std::array<std::uint8_t, kSendBufferSize> buffer {};
    const Decoded<std::size_t> size = xnl::serialize_frame(frame, buffer);
    if (!size.has_value())
    {
        return from_protocol(size.error(), "serialising a frame");
    }

    if (!mStream->sendAll(std::span<const std::uint8_t>(buffer.data(), *size)))
    {
        dropConnectionLocked("send failed");
        return io_error("send failed");
    }

    return {};
}

Result<xnl::Frame> Radio::readFrameLocked(std::chrono::steady_clock::time_point deadline)
{
    for (;;)
    {
        const Decoded<std::size_t> needed = xnl::frame_length(mRxBuffer);
        if (needed.has_value() && mRxBuffer.size() >= *needed)
        {
            // Move the frame out before parsing it: the parsed payload is a
            // view, and consuming the buffer underneath it would leave that
            // view pointing at the next frame's bytes.
            const auto begin = mRxBuffer.begin();
            mFrame.assign(begin, begin + static_cast<std::ptrdiff_t>(*needed));
            mRxBuffer.erase(begin, begin + static_cast<std::ptrdiff_t>(*needed));

            const Decoded<xnl::Frame> parsed = xnl::parse_frame(mFrame);
            if (parsed.has_value())
            {
                return *parsed;
            }

            // The length field was usable even though the body was not, so the
            // frame boundary is known and the stream resynchronises by itself.
            ++mStats.decodeErrors;
            continue;
        }

        if (!mStream)
        {
            return not_connected("no stream");
        }

        const unsigned budget = millisecondsUntil(deadline);
        if (budget == 0)
        {
            return timeout("no frame arrived before the deadline");
        }

        std::array<std::uint8_t, kReadChunk> chunk {};
        const ssize_t read = mStream->recvSome(chunk, budget);
        if (read < 0)
        {
            dropConnectionLocked("the radio closed the connection");
            return io_error("the radio closed the connection");
        }
        if (read == 0)
        {
            continue;
        }

        if (mRxBuffer.size() + static_cast<std::size_t>(read) > kMaxBufferedBytes)
        {
            dropConnectionLocked("the stream is not XNL: no frame boundary found");
            return protocol_error("the stream is not XNL: no frame boundary found");
        }

        mRxBuffer.insert(mRxBuffer.end(), chunk.begin(), chunk.begin() + read);
    }
}

Result<xnl::Frame> Radio::readFrameOfLocked(xnl::Opcode wanted, std::chrono::steady_clock::time_point deadline)
{
    for (;;)
    {
        Result<xnl::Frame> frame = readFrameLocked(deadline);
        if (!frame.has_value())
        {
            return frame;
        }
        if (frame->opcode == wanted)
        {
            return frame;
        }

        // Anything else during the handshake is a broadcast or a sysmap
        // message. Queue what we understand, count the rest, and keep waiting
        // -- bounded by the deadline rather than by an attempt count.
        if (queueBroadcastLocked(*frame))
        {
            continue;
        }

        ++mStats.framesSkipped;
    }
}

bool Radio::queueBroadcastLocked(const xnl::Frame& frame)
{
    if (frame.opcode != xnl::Opcode::DataMessage)
    {
        return false;
    }

    const Decoded<xcmp::Message> message = xcmp::parse_message(frame.payload);
    if (!message.has_value() || !message->isBroadcast())
    {
        return false;
    }

    const Decoded<control::Broadcast> decoded = control::parse_broadcast(*message);
    if (!decoded.has_value())
    {
        ++mStats.decodeErrors;
        return false;
    }

    Broadcast out;
    out.opcode = decoded->opcode;
    out.name = control::broadcast_name(decoded->opcode);
    out.payload.assign(decoded->payload.begin(), decoded->payload.end());
    out.zoneChannel = decoded->zoneChannel;

    if (decoded->display.has_value())
    {
        out.display = Broadcast::DisplayLine { decoded->display->line, decoded->display->flags,
                                               control::decode_display_text(decoded->display->encodedText) };
    }

    if (mBroadcasts.size() >= mOptions.maxQueuedBroadcasts)
    {
        mBroadcasts.pop_front();
        ++mStats.broadcastsDropped;
    }

    mBroadcasts.push_back(std::move(out));
    ++mStats.broadcasts;
    return true;
}

Result<std::vector<std::uint8_t>> Radio::transact(const xcmp::Command& command)
{
    const std::lock_guard<std::mutex> lock(mMutex);
    return transactLocked(command);
}

Result<std::vector<std::uint8_t>> Radio::transactLocked(const xcmp::Command& command)
{
    // A query does NOT open the session. Connecting takes as long as the
    // connect timeout, and doing it here means a service call arriving while
    // the radio is unplugged blocks its caller for that long and holds this
    // lock while it does -- so the whole node stalls on somebody's poll.
    // Reconnection belongs to pump(), which is called on a loop and has
    // nothing else to do; a query while the session is down fails at once,
    // with the reason the last attempt failed.
    if (!mConnected || !mStream || !mStream->isOpen())
    {
        return not_connected(mStats.lastError.empty() ? "the session is not up" : mStats.lastError);
    }

    const std::uint16_t transaction = nextTransactionLocked();

    if (const Result<void> sent = sendFrameLocked(xnl::Opcode::DataMessage, 1, nextFlagsLocked(),
                                                  mMasterAddress, mAddress, transaction, command.bytes());
        !sent.has_value())
    {
        return std::unexpected(sent.error());
    }

    ++mStats.commands;

    const auto deadline = std::chrono::steady_clock::now() + mOptions.replyTimeout;
    const std::uint16_t wanted = command.replyOpcode();

    for (;;)
    {
        const Result<xnl::Frame> frame = readFrameLocked(deadline);
        if (!frame.has_value())
        {
            return std::unexpected(frame.error());
        }

        if (frame->opcode != xnl::Opcode::DataMessage)
        {
            ++mStats.framesSkipped;
            continue;
        }

        const Decoded<xcmp::Message> message = xcmp::parse_message(frame->payload);
        if (!message.has_value())
        {
            ++mStats.decodeErrors;
            continue;
        }

        if (message->isBroadcast())
        {
            queueBroadcastLocked(*frame);
            continue;
        }

        // CORRELATE ON BOTH the echoed transaction id and the reply opcode.
        // Neither alone is enough: several distinct queries share one opcode
        // (0x000E picks the item with a payload byte, so model, serial and
        // radio id all reply 0x800E), and a reply to a query that already
        // timed out carries a transaction we are no longer waiting for.
        if (frame->transaction != transaction || message->opcode != wanted)
        {
            ++mStats.framesSkipped;
            continue;
        }

        ++mStats.replies;

        const Decoded<std::span<const std::uint8_t>> body = xcmp::reply_body(*message);
        if (!body.has_value())
        {
            return from_protocol(body.error(), "reply to XCMP 0x" + std::to_string(command.opcode()));
        }

        return std::vector<std::uint8_t>(body->begin(), body->end());
    }
}

void Radio::pump(std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    {
        const std::lock_guard<std::mutex> lock(mMutex);
        if (const Result<void> ready = ensureConnectedLocked(false); !ready.has_value())
        {
            // Fall through to the wait below. Returning immediately would hand
            // the caller's loop back with nothing to do and no delay, which
            // spins a core for as long as the radio is unplugged.
            mPumpIdle = true;
        }
        else
        {
            mPumpIdle = false;
            pumpLocked(deadline);
        }
    }

    if (mPumpIdle)
    {
        // Waited WITHOUT the lock, so a service call is not stuck behind a
        // radio that is not there.
        std::this_thread::sleep_until(deadline);
    }
}

void Radio::pumpLocked(std::chrono::steady_clock::time_point deadline)
{
    for (;;)
    {
        const Result<xnl::Frame> frame = readFrameLocked(deadline);
        if (!frame.has_value())
        {
            // A timeout is the normal way out: it means the radio had nothing
            // to say. Anything else has already dropped the session.
            return;
        }

        if (frame->opcode != xnl::Opcode::DataMessage)
        {
            ++mStats.framesSkipped;
            continue;
        }

        if (!queueBroadcastLocked(*frame))
        {
            // A reply to a query that gave up before it arrived.
            ++mStats.framesSkipped;
        }
    }
}

std::vector<Broadcast> Radio::takeBroadcasts()
{
    const std::lock_guard<std::mutex> lock(mMutex);

    std::vector<Broadcast> out;
    out.reserve(mBroadcasts.size());
    for (Broadcast& broadcast : mBroadcasts)
    {
        out.push_back(std::move(broadcast));
    }
    mBroadcasts.clear();
    return out;
}

// ===========================================================================
// The typed queries
// ===========================================================================

Result<control::ZoneChannel> Radio::channel()
{
    const Result<std::vector<std::uint8_t>> body = transact(control::query_channel());
    if (!body.has_value())
    {
        return std::unexpected(body.error());
    }

    const Decoded<control::ChannelReply> reply = control::parse_channel_reply(*body);
    if (!reply.has_value())
    {
        return from_protocol(reply.error(), "channel query");
    }

    return control::ZoneChannel { reply->zone, reply->channel };
}

Result<Radio::ChannelCounts> Radio::channelCounts()
{
    const Result<std::vector<std::uint8_t>> zones = transact(control::query_zone_count());
    if (!zones.has_value())
    {
        return std::unexpected(zones.error());
    }

    const Decoded<control::ChannelReply> zoneReply = control::parse_channel_reply(*zones);
    if (!zoneReply.has_value())
    {
        return from_protocol(zoneReply.error(), "zone count");
    }

    const Result<std::vector<std::uint8_t>> channels = transact(control::query_channel_count());
    if (!channels.has_value())
    {
        return std::unexpected(channels.error());
    }

    const Decoded<control::ChannelReply> channelReply = control::parse_channel_reply(*channels);
    if (!channelReply.has_value())
    {
        return from_protocol(channelReply.error(), "channel count");
    }

    // The counts arrive in different halves of the same reply shape: 0x81
    // answers in the first field, 0x82 in the second.
    return ChannelCounts { zoneReply->zone, channelReply->channel };
}

Result<control::ZoneChannel> Radio::stepChannel(bool up)
{
    const Result<std::vector<std::uint8_t>> body =
        transact(up ? control::channel_up() : control::channel_down());
    if (!body.has_value())
    {
        return std::unexpected(body.error());
    }

    const Decoded<control::ChannelReply> reply = control::parse_channel_reply(*body);
    if (!reply.has_value())
    {
        return from_protocol(reply.error(), "channel step");
    }

    return control::ZoneChannel { reply->zone, reply->channel };
}

Result<control::ZoneChannel> Radio::selectChannel(std::uint16_t zone, std::uint16_t channelIndex)
{
    const Result<control::ZoneChannel> current = channel();
    if (!current.has_value())
    {
        return current;
    }

    if (current->zone != zone)
    {
        return not_permitted("this radio cannot change zone over XNL (on zone " +
                             std::to_string(current->zone) + ", asked for " + std::to_string(zone) + ")");
    }

    if (current->channel == channelIndex)
    {
        return *current;
    }

    const Result<ChannelCounts> counts = channelCounts();
    if (!counts.has_value())
    {
        return std::unexpected(counts.error());
    }

    if (counts->channelsInZone == 0)
    {
        return protocol_error("the radio reports no channels in the current zone");
    }

    // Always upwards: the radio wraps at the end of the zone, so one direction
    // reaches everything, and the worst case on a zone of five is four steps.
    std::uint16_t previous = current->channel;
    for (std::uint16_t step = 0; step < counts->channelsInZone; ++step)
    {
        const Result<control::ZoneChannel> now = stepChannel(true);
        if (!now.has_value())
        {
            return now;
        }

        if (now->zone == zone && now->channel == channelIndex)
        {
            return now;
        }

        // The radio reports success for a step it did not take, so a channel
        // that did not move is the only signal that stepping has stopped
        // working -- without this the loop would run the full count and blame
        // the target instead.
        if (now->channel == previous)
        {
            return protocol_error("the radio stopped changing channel at " + std::to_string(previous));
        }

        previous = now->channel;
    }

    return invalid_argument("channel " + std::to_string(channelIndex) + " was not reached after " +
                            std::to_string(counts->channelsInZone) + " steps");
}

Result<Radio::StatusValue> Radio::status(control::StatusItem item)
{
    const control::StatusItemSpec* spec = control::find_status_item(item);
    if (spec == nullptr)
    {
        return invalid_argument("unknown status item 0x" +
                                std::to_string(static_cast<unsigned>(item)));
    }

    // Refused here rather than asked. An XPR 5550 mobile answers every station
    // meter with a result code, and a node polling one would report a radio
    // fault once a second for a question this radio was never going to answer.
    if (!spec->supported)
    {
        return not_permitted(std::string(spec->name) + " is not supported on this radio");
    }

    const Result<std::vector<std::uint8_t>> body = transact(control::radio_status(item));
    if (!body.has_value())
    {
        return std::unexpected(body.error());
    }

    const Decoded<control::StatusReading> reading = control::parse_status(item, *body);
    if (!reading.has_value())
    {
        return from_protocol(reading.error(), std::string("status ") + std::string(spec->name));
    }

    StatusValue value;
    value.item = item;
    value.encoding = spec->encoding;
    value.raw.assign(reading->value.begin(), reading->value.end());

    switch (spec->encoding)
    {
        case control::StatusEncoding::Ascii:
            value.text = control::ascii_text(reading->value);
            break;
        case control::StatusEncoding::Unsigned:
            if (const Decoded<std::uint32_t> number = control::status_unsigned(reading->value);
                number.has_value())
            {
                value.number = *number;
                value.hasNumber = true;
            }
            break;
        case control::StatusEncoding::Opaque:
            break;
    }

    return value;
}

Result<Radio::Identity> Radio::identity()
{
    Identity out;

    // Best effort past the first query: which items a radio answers varies by
    // model, and a half-known identity is more useful than none. A failure of
    // the SESSION, though, is reported -- an empty identity because the radio
    // is unplugged must not look like an empty identity because it is coy.
    const Result<StatusValue> model = status(control::StatusItem::ModelNumber);
    if (!model.has_value())
    {
        if (isSessionFailure(model.error().kind))
        {
            return std::unexpected(model.error());
        }
    }
    else
    {
        out.modelNumber = model->text;
    }

    if (const Result<StatusValue> serial = status(control::StatusItem::SerialNumber); serial.has_value())
    {
        out.serialNumber = serial->text;
    }

    if (const Result<StatusValue> radioId = status(control::StatusItem::RadioId);
        radioId.has_value() && radioId->hasNumber)
    {
        out.radioId = radioId->number;
        out.radioIdKnown = true;
    }

    if (const Result<std::vector<std::uint8_t>> version = transact(control::version_info());
        version.has_value())
    {
        out.firmwareVersion = control::ascii_text(*version);
    }

    if (const Result<std::vector<std::uint8_t>> tanapa = transact(control::tanapa_number());
        tanapa.has_value())
    {
        out.tanapaNumber = control::ascii_text(*tanapa);
    }

    if (const Result<std::vector<std::uint8_t>> date = transact(control::datecode()); date.has_value())
    {
        out.datecode = *date;
    }

    return out;
}

} // namespace xpr
