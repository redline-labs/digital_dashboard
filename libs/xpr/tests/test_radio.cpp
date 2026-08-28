// SPDX-License-Identifier: GPL-3.0-or-later
//
// The XNL session end to end, against a scripted radio in memory.
//
// No sockets: the fake radio below plays the master's half of the handshake,
// including the real TEA challenge/response, so this is a `unit` test that
// nevertheless exercises everything between "open a stream" and "the channel
// changed".
//
// FIVE OF THE ASSERTIONS HERE ARE REGRESSION TESTS FOR DEFECTS A CAPTURE COULD
// NOT REVEAL, because a capture only ever shows traffic that WORKED, and our
// wrong version merely produced silence. The fake radio enforces them the way the real one
// does -- by refusing to answer:
//
//   1. CONN_REQUEST is twelve bytes, with the device type at +2.
//   2. The assigned address is read from CONN_REPLY+2, not +0.
//   3. The data-message flags counter must advance on every message.
//   4. Unacknowledged delivery must be selected in CONN_REQUEST's flags.
//   5. Replies are correlated by transaction id, not by opcode alone.
//
// The fifth is the one worth reading the fake radio for: it answers two
// different questions with the same opcode, in the wrong order, which is
// exactly what an opcode-only correlation gets wrong.

#include "xpr/radio.h"

#include "mototrbo/control.h"
#include "mototrbo/xcmp.h"
#include "mototrbo/xnl.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace
{

int failures = 0;

void check(bool condition, const std::string& what)
{
    if (!condition)
    {
        SPDLOG_ERROR("FAIL: {}", what);
        ++failures;
    }
}

using namespace mototrbo;

constexpr std::uint16_t kMasterAddress = 0x0100;
constexpr std::uint16_t kTemporaryAddress = 0x000A;
constexpr std::uint16_t kAssignedAddress = 0x0004;

// ---------------------------------------------------------------------------
// An in-memory stream: whatever the client writes goes to a responder, and
// whatever the responder returns becomes readable.
// ---------------------------------------------------------------------------
class LoopbackStream final : public xpr::ByteStream
{
  public:
    using Responder = std::function<std::vector<std::uint8_t>(std::span<const std::uint8_t>)>;

    explicit LoopbackStream(Responder responder) : mResponder(std::move(responder)) {}

    void preload(std::span<const std::uint8_t> bytes)
    {
        mPending.insert(mPending.end(), bytes.begin(), bytes.end());
    }

    bool sendAll(std::span<const std::uint8_t> data) override
    {
        if (!mOpen)
        {
            return false;
        }

        const std::vector<std::uint8_t> reply = mResponder(data);
        mPending.insert(mPending.end(), reply.begin(), reply.end());
        return true;
    }

    ssize_t recvSome(std::span<std::uint8_t> out, unsigned timeoutMs) override
    {
        if (!mOpen)
        {
            return -1;
        }

        if (mPending.empty())
        {
            // Sleeping rather than returning immediately keeps a caller that
            // is waiting out a deadline from spinning on the CPU, which is
            // what a socket would do.
            std::this_thread::sleep_for(std::chrono::milliseconds(std::min(timeoutMs, 20u)));
            return 0;
        }

        const std::size_t count = std::min(out.size(), mPending.size());
        for (std::size_t i = 0; i < count; ++i)
        {
            out[i] = mPending.front();
            mPending.pop_front();
        }

        return static_cast<ssize_t>(count);
    }

    bool isOpen() const override { return mOpen; }

    void close() override { mOpen = false; }

  private:
    Responder mResponder;
    std::deque<std::uint8_t> mPending;
    bool mOpen { true };
};

std::vector<std::uint8_t> serialize(const xnl::Frame& frame)
{
    std::array<std::uint8_t, 512> buffer {};
    const std::size_t size = xnl::serialize_frame(frame, buffer).value_or(0);
    return std::vector<std::uint8_t>(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(size));
}

// ---------------------------------------------------------------------------
// The scripted radio.
// ---------------------------------------------------------------------------
struct FakeRadio
{
    // What the client got wrong, if anything. Checked after the exchange so a
    // failure names the defect rather than showing up as a timeout.
    std::string complaint;

    std::array<std::uint8_t, 8> challenge {};
    bool unacknowledgedSelected { false };
    int dataMessages { 0 };
    unsigned lastFlags { 0xFF };
    bool rejectAuthentication { false };

    std::uint16_t zone { 1 };
    std::uint16_t channel { 2 };
    std::uint16_t channelCount { 5 };
    // Set to freeze the channel: the radio then reports success for a step it
    // did not take, which is a real behaviour of the direct-select operation.
    bool ignoreSteps { false };

    // Queued ahead of the next reply, so a broadcast arrives INTERLEAVED with
    // a command exchange -- the case that makes a query eat a broadcast.
    std::vector<std::uint8_t> pendingBroadcast;

    // The acknowledged-delivery failure, reproduced: a radio that does not get
    // its DATA_MSG_ACK retransmits each reply five times, so the PREVIOUS
    // query's answer is sitting in the buffer when the next query is sent. It
    // carries the previous transaction id, which is the only thing that
    // distinguishes it -- both replies have the same opcode.
    std::vector<std::uint8_t> previousReplyFrame;
    bool retransmitPreviousReply { true };

    std::uint8_t statusResultCode { 0x00 };

    void fail(std::string what)
    {
        if (complaint.empty())
        {
            complaint = std::move(what);
        }
    }

    std::vector<std::uint8_t> operator()(std::span<const std::uint8_t> sent)
    {
        const Result<xnl::Frame> in = xnl::parse_frame(sent);
        if (!in.has_value())
        {
            fail("the client sent a frame that does not parse");
            return {};
        }

        xnl::Frame out;
        out.dst = kTemporaryAddress;
        out.src = kMasterAddress;
        out.transaction = in->transaction;

        switch (in->opcode)
        {
            case xnl::Opcode::DeviceMasterQuery:
                out.opcode = xnl::Opcode::MasterStatusBroadcast;
                return serialize(out);

            case xnl::Opcode::DeviceAuthRequest:
            {
                out.opcode = xnl::Opcode::DeviceAuthReply;
                out.payload = {};
                std::vector<std::uint8_t> payload { 0x00, 0x0A };
                for (std::size_t i = 0; i < challenge.size(); ++i)
                {
                    challenge[i] = static_cast<std::uint8_t>(0x10 + i);
                    payload.push_back(challenge[i]);
                }
                out.payload = payload;
                return serialize(out);
            }

            case xnl::Opcode::DeviceConnRequest:
                return connReply(*in);

            case xnl::Opcode::DataMessage:
            {
                // The radio dedupes on this counter: held constant, every
                // message after the first is discarded as a retransmission.
                if (in->flags == lastFlags)
                {
                    fail("the data message flags counter did not advance");
                }
                lastFlags = in->flags;
                ++dataMessages;
                return dataReply(*in);
            }

            case xnl::Opcode::MasterStatusBroadcast:
            case xnl::Opcode::DeviceAuthReply:
            case xnl::Opcode::DeviceConnReply:
            case xnl::Opcode::DeviceSysmapRequest:
            case xnl::Opcode::DeviceSysmapBroadcast:
            case xnl::Opcode::DataMessageAck:
                break;
        }

        return {};
    }

    std::vector<std::uint8_t> connReply(const xnl::Frame& in)
    {
        if (in.payload.size() != xnl::kConnRequestPayloadSize)
        {
            // The real radio's response to this is silence, which is what made
            // it expensive to find.
            fail("CONN_REQUEST must be 12 bytes, not " + std::to_string(in.payload.size()));
            return {};
        }
        if (in.payload[2] != xnl::kDeviceType)
        {
            fail("CONN_REQUEST must carry the device type at offset 2");
            return {};
        }
        if ((in.flags & xnl::kConnFlagUnacked) == 0)
        {
            fail("the client must select unacknowledged delivery");
            return {};
        }

        unacknowledgedSelected = true;

        const std::array<std::uint8_t, 8> expected = xnl::auth_response(challenge);
        const bool authenticated =
            !rejectAuthentication && std::equal(expected.begin(), expected.end(), in.payload.begin() + 4);

        xnl::Frame out;
        out.opcode = xnl::Opcode::DeviceConnReply;
        out.dst = kTemporaryAddress;
        out.src = kMasterAddress;
        out.transaction = in.transaction;

        std::vector<std::uint8_t> payload;
        if (!authenticated)
        {
            // A rejected authentication is an all-zero payload, so a zero
            // assigned address is the only signal.
            payload.assign(14, 0x00);
            out.payload = payload;
            return serialize(out);
        }

        payload = { 0x01, 0x05,
                    static_cast<std::uint8_t>(kAssignedAddress >> 8),
                    static_cast<std::uint8_t>(kAssignedAddress & 0xFF),
                    xnl::kDeviceType, 0x01 };
        payload.resize(14, 0xA0);
        out.payload = payload;
        return serialize(out);
    }

    std::vector<std::uint8_t> reply(const xnl::Frame& in, std::vector<std::uint8_t> message)
    {
        xnl::Frame out;
        out.opcode = xnl::Opcode::DataMessage;
        out.protocol = 1;
        out.dst = kAssignedAddress;
        out.src = kMasterAddress;
        out.transaction = in.transaction;
        out.payload = message;

        std::vector<std::uint8_t> bytes;
        if (retransmitPreviousReply && !previousReplyFrame.empty())
        {
            bytes = previousReplyFrame;
        }
        if (!pendingBroadcast.empty())
        {
            bytes.insert(bytes.end(), pendingBroadcast.begin(), pendingBroadcast.end());
            pendingBroadcast.clear();
        }

        const std::vector<std::uint8_t> serialized = serialize(out);
        previousReplyFrame = serialized;
        bytes.insert(bytes.end(), serialized.begin(), serialized.end());
        return bytes;
    }

    std::vector<std::uint8_t> dataReply(const xnl::Frame& in)
    {
        const Result<xcmp::Message> request = xcmp::parse_message(in.payload);
        if (!request.has_value())
        {
            fail("the client sent an XCMP message shorter than an opcode");
            return {};
        }

        const std::uint16_t replyOpcode = request->opcode | xcmp::kReplyBit;
        std::vector<std::uint8_t> message { static_cast<std::uint8_t>(replyOpcode >> 8),
                                            static_cast<std::uint8_t>(replyOpcode & 0xFF) };

        if (request->opcode == static_cast<std::uint16_t>(xcmp::Opcode::ZoneChannel))
        {
            const auto operation = static_cast<control::ChannelOp>(request->payload[0]);
            if (operation == control::ChannelOp::Up && !ignoreSteps)
            {
                channel = static_cast<std::uint16_t>(channel % channelCount + 1);
            }
            else if (operation == control::ChannelOp::Down && !ignoreSteps)
            {
                channel = static_cast<std::uint16_t>(channel == 1 ? channelCount : channel - 1);
            }

            std::uint16_t first = zone;
            std::uint16_t second = channel;
            if (operation == control::ChannelOp::ZoneCount)
            {
                first = 2;
                second = 0;
            }
            else if (operation == control::ChannelOp::ChannelCount)
            {
                first = 0;
                second = channelCount;
            }

            message.push_back(0x00); // result: success
            message.push_back(static_cast<std::uint8_t>(operation));
            message.push_back(static_cast<std::uint8_t>(first >> 8));
            message.push_back(static_cast<std::uint8_t>(first & 0xFF));
            message.push_back(static_cast<std::uint8_t>(second >> 8));
            message.push_back(static_cast<std::uint8_t>(second & 0xFF));
            return reply(in, std::move(message));
        }

        if (request->opcode == static_cast<std::uint16_t>(xcmp::Opcode::RadioStatus))
        {
            const std::uint8_t item = request->payload[0];
            message.push_back(statusResultCode);
            if (statusResultCode == 0x00)
            {
                message.push_back(item);
                if (item == static_cast<std::uint8_t>(control::StatusItem::ModelNumber))
                {
                    for (const char character : std::string("M28TRN9WA1AN"))
                    {
                        message.push_back(static_cast<std::uint8_t>(character));
                    }
                }
                else if (item == static_cast<std::uint8_t>(control::StatusItem::SerialNumber))
                {
                    for (const char character : std::string("511TVMG951"))
                    {
                        message.push_back(static_cast<std::uint8_t>(character));
                    }
                }
                else if (item == static_cast<std::uint8_t>(control::StatusItem::RadioId))
                {
                    message.insert(message.end(), { 0x00, 0x00, 0x23, 0xD8 });
                }
            }

            return reply(in, std::move(message));
        }

        // Everything else: an empty success.
        message.push_back(0x00);
        return reply(in, std::move(message));
    }
};

// A 0xB40D zone/channel broadcast, framed as the radio sends it: no
// transaction of ours, arriving in the middle of somebody's query.
std::vector<std::uint8_t> channelBroadcastFrame(std::uint16_t zone, std::uint16_t channel)
{
    const std::vector<std::uint8_t> message { 0xB4, 0x0D,
                                              static_cast<std::uint8_t>(zone >> 8),
                                              static_cast<std::uint8_t>(zone & 0xFF),
                                              static_cast<std::uint8_t>(channel >> 8),
                                              static_cast<std::uint8_t>(channel & 0xFF) };

    xnl::Frame frame;
    frame.opcode = xnl::Opcode::DataMessage;
    frame.protocol = 1;
    frame.dst = kAssignedAddress;
    frame.src = kMasterAddress;
    frame.transaction = 0x7FFF;
    frame.payload = message;
    return serialize(frame);
}

// Builds a Radio around a fresh fake. The fake outlives the radio because the
// caller owns it.
std::unique_ptr<xpr::Radio> makeRadio(FakeRadio& fake, xpr::Radio::Options options = {})
{
    options.replyTimeout = std::chrono::milliseconds(500);
    options.handshakeTimeout = std::chrono::milliseconds(500);

    return std::make_unique<xpr::Radio>(
        [&fake]() -> xpr::Result<std::unique_ptr<xpr::ByteStream>> {
            auto stream = std::make_unique<LoopbackStream>(
                [&fake](std::span<const std::uint8_t> sent) { return fake(sent); });

            // The radio broadcasts its master status a few milliseconds after
            // accepting the connection, before anything is sent.
            xnl::Frame announcement;
            announcement.opcode = xnl::Opcode::MasterStatusBroadcast;
            announcement.src = kMasterAddress;
            announcement.dst = 0xFFFF;
            const std::vector<std::uint8_t> bytes = serialize(announcement);
            stream->preload(bytes);

            return std::unique_ptr<xpr::ByteStream>(std::move(stream));
        },
        options);
}

void checkHandshake()
{
    FakeRadio fake;
    const std::unique_ptr<xpr::Radio> radio = makeRadio(fake);

    const xpr::Result<void> connected = radio->connect();
    check(connected.has_value(), "handshake completes");
    if (!connected.has_value())
    {
        SPDLOG_ERROR("  {}", xpr::to_string(connected.error()));
    }

    check(fake.complaint.empty(), "the radio had no complaint: " + fake.complaint);
    check(fake.unacknowledgedSelected, "unacknowledged delivery was selected");
    // Read from CONN_REPLY+2. Reading it from +0 yields 0x0105 here.
    check(radio->stats().address == kAssignedAddress, "the assigned address comes from CONN_REPLY+2");
}

void checkAuthenticationRejected()
{
    FakeRadio fake;
    fake.rejectAuthentication = true;
    const std::unique_ptr<xpr::Radio> radio = makeRadio(fake);

    const xpr::Result<void> connected = radio->connect();
    check(!connected.has_value(), "a rejected authentication fails the handshake");
    if (!connected.has_value())
    {
        check(connected.error().kind == xpr::Error::Kind::AuthFailed,
              "an all-zero CONN_REPLY reports as an authentication failure");
    }
    check(!radio->connected(), "a failed handshake leaves the session down");
}

void checkQueriesAreCorrelated()
{
    FakeRadio fake;
    const std::unique_ptr<xpr::Radio> radio = makeRadio(fake);
    check(radio->connect().has_value(), "connect for the query test");

    // Both of these are XCMP 0x000E and both reply 0x800E. Correlation by
    // opcode alone hands the second query the first one's answer.
    const xpr::Result<xpr::Radio::StatusValue> model =
        radio->status(control::StatusItem::ModelNumber);
    const xpr::Result<xpr::Radio::StatusValue> serial =
        radio->status(control::StatusItem::SerialNumber);

    check(model.has_value() && model->text == "M28TRN9WA1AN", "model number");
    // If this reads "M28TRN9WA1AN" the reply stream is one behind: the fake
    // radio retransmits the previous answer ahead of this one, exactly as a
    // real one does when it is not acknowledged, and both answers carry
    // opcode 0x800E.
    check(serial.has_value() && serial->text == "511TVMG951", "serial number");

    const xpr::Result<xpr::Radio::StatusValue> radioId = radio->status(control::StatusItem::RadioId);
    check(radioId.has_value() && radioId->hasNumber && radioId->number == 9176, "radio id decodes as a u32");

    // An item this radio does not answer is refused locally, without a round
    // trip: asking costs a reply timeout and gets a result code anyway.
    const int before = fake.dataMessages;
    const xpr::Result<xpr::Radio::StatusValue> uptime = radio->status(control::StatusItem::Uptime);
    check(!uptime.has_value() && uptime.error().kind == xpr::Error::Kind::NotPermitted,
          "an unsupported status item is refused locally");
    check(fake.dataMessages == before, "an unsupported item costs no round trip");

    check(fake.complaint.empty(), "the radio had no complaint: " + fake.complaint);
}

void checkRefusalCarriesTheResultCode()
{
    FakeRadio fake;
    fake.statusResultCode = static_cast<std::uint8_t>(xcmp::ResultCode::OpcodeUnsupported);
    const std::unique_ptr<xpr::Radio> radio = makeRadio(fake);
    check(radio->connect().has_value(), "connect for the refusal test");

    const xpr::Result<xpr::Radio::StatusValue> rssi = radio->status(control::StatusItem::Rssi);
    check(!rssi.has_value(), "a non-zero result code is an error, not a value");
    if (!rssi.has_value())
    {
        // "Refused" and "protocol error" are the two ends of the diagnostic:
        // one says the radio will never do this, the other says something is
        // broken. Flattening them together is what makes a refusal look like
        // a bug.
        check(rssi.error().kind == xpr::Error::Kind::Refused, "a refusal reports as Refused");
        check(rssi.error().code == static_cast<int>(xcmp::ResultCode::OpcodeUnsupported),
              "the radio's own result code survives");
    }
}

void checkChannelControl()
{
    FakeRadio fake;
    const std::unique_ptr<xpr::Radio> radio = makeRadio(fake);
    check(radio->connect().has_value(), "connect for the channel test");

    const xpr::Result<control::ZoneChannel> where = radio->channel();
    check(where.has_value() && where->zone == 1 && where->channel == 2, "channel query");

    const xpr::Result<xpr::Radio::ChannelCounts> counts = radio->channelCounts();
    check(counts.has_value() && counts->zones == 2 && counts->channelsInZone == 5,
          "the counts come out of different halves of the same reply shape");

    const xpr::Result<control::ZoneChannel> stepped = radio->stepChannel(true);
    check(stepped.has_value() && stepped->channel == 3, "a step moves the radio");

    // Stepping is how a channel is selected: direct select is inert on this
    // radio, so selectChannel steps until it arrives.
    const xpr::Result<control::ZoneChannel> selected = radio->selectChannel(1, 5);
    check(selected.has_value() && selected->channel == 5, "select steps to the target");

    // And it wraps: 5 -> 1 is one step up on a zone of five.
    const xpr::Result<control::ZoneChannel> wrapped = radio->selectChannel(1, 1);
    check(wrapped.has_value() && wrapped->channel == 1, "select wraps at the end of the zone");

    // A zone this radio is not on is refused rather than attempted -- zone
    // cannot be changed over XNL at all, and stepping in the hope of crossing
    // a boundary would be a guess with somebody's radio.
    const xpr::Result<control::ZoneChannel> otherZone = radio->selectChannel(2, 1);
    check(!otherZone.has_value() && otherZone.error().kind == xpr::Error::Kind::NotPermitted,
          "a zone change is refused");

    check(fake.complaint.empty(), "the radio had no complaint: " + fake.complaint);
}

void checkSelectGivesUpWhenTheRadioStops()
{
    FakeRadio fake;
    fake.ignoreSteps = true;
    const std::unique_ptr<xpr::Radio> radio = makeRadio(fake);
    check(radio->connect().has_value(), "connect for the stuck-channel test");

    // The radio reports success for a step it did not take. Without the
    // did-it-move guard this would run the full channel count and then blame
    // the target channel for not existing.
    const xpr::Result<control::ZoneChannel> selected = radio->selectChannel(1, 4);
    check(!selected.has_value(), "a radio that will not step is an error");
    if (!selected.has_value())
    {
        check(selected.error().kind == xpr::Error::Kind::Protocol,
              "a radio that stops stepping reports as a protocol error");
    }
}

void checkBroadcastsSurviveAQuery()
{
    FakeRadio fake;
    // Queued ahead of the reply to the next command, so it arrives while the
    // client is waiting for something else -- which is exactly when a channel
    // change happens.
    fake.pendingBroadcast = channelBroadcastFrame(1, 4);

    const std::unique_ptr<xpr::Radio> radio = makeRadio(fake);
    check(radio->connect().has_value(), "connect for the broadcast test");

    const xpr::Result<control::ZoneChannel> where = radio->channel();
    check(where.has_value() && where->channel == 2, "the query still got its own reply");

    const std::vector<xpr::Broadcast> broadcasts = radio->takeBroadcasts();
    check(broadcasts.size() == 1, "a broadcast that arrived during a query is kept, not eaten");
    if (broadcasts.size() == 1)
    {
        check(broadcasts[0].opcode == 0xB40D, "broadcast opcode");
        check(broadcasts[0].name == "zone/channel", "broadcast name");
        check(broadcasts[0].zoneChannel.has_value() && broadcasts[0].zoneChannel->channel == 4,
              "broadcast decodes to a zone and channel");
    }

    check(radio->takeBroadcasts().empty(), "the queue is emptied by taking it");
}

void checkBroadcastQueueIsBounded()
{
    FakeRadio fake;
    xpr::Radio::Options options;
    options.maxQueuedBroadcasts = 2;
    const std::unique_ptr<xpr::Radio> radio = makeRadio(fake, options);
    check(radio->connect().has_value(), "connect for the queue bound test");

    for (std::uint16_t channel = 1; channel <= 4; ++channel)
    {
        fake.pendingBroadcast = channelBroadcastFrame(1, channel);
        (void)radio->channel();
    }

    const std::vector<xpr::Broadcast> broadcasts = radio->takeBroadcasts();
    check(broadcasts.size() == 2, "the queue is bounded");
    // Oldest first: a display line from ten seconds ago is worth less than the
    // current one.
    check(!broadcasts.empty() && broadcasts.back().zoneChannel->channel == 4,
          "the newest broadcast is the one kept");
    check(radio->stats().broadcastsDropped == 2, "dropped broadcasts are counted, not hidden");
}

void checkNotConnected()
{
    FakeRadio fake;
    xpr::Radio::Options options;
    // No backoff: the failure must be reported on the spot rather than
    // deferred to a schedule.
    options.reconnectBackoff.clear();

    xpr::Radio radio(
        []() -> xpr::Result<std::unique_ptr<xpr::ByteStream>> {
            return xpr::connect_failed("nothing is listening", 0);
        },
        options);

    const xpr::Result<void> connected = radio.connect();
    check(!connected.has_value(), "connecting to nothing fails");
    check(!radio.connected(), "and the session stays down");
    check(radio.stats().connectFailures == 1, "the failure is counted");
    check(!radio.stats().lastError.empty(), "and the reason is kept for the status topic");

    // A query does not try to connect. It reports the reason the last attempt
    // failed and returns immediately -- opening a session here would block the
    // caller for the connect timeout and hold the radio's lock while it did.
    const xpr::Result<control::ZoneChannel> where = radio.channel();
    check(!where.has_value(), "a query on a down session fails");
    if (!where.has_value())
    {
        check(where.error().kind == xpr::Error::Kind::NotConnected, "and says so");
    }
    check(radio.stats().connectFailures == 1, "and did not try to connect on its own");

    // pump() must not throw or block when there is nothing to talk to.
    radio.pump(std::chrono::milliseconds(1));
}

} // namespace

int main()
{
    spdlog::set_pattern("[%^%l%$] %v");
    spdlog::set_level(spdlog::level::warn);

    checkHandshake();
    checkAuthenticationRejected();
    checkQueriesAreCorrelated();
    checkRefusalCarriesTheResultCode();
    checkChannelControl();
    checkSelectGivesUpWhenTheRadioStops();
    checkBroadcastsSurviveAQuery();
    checkBroadcastQueueIsBounded();
    checkNotConnected();

    if (failures != 0)
    {
        SPDLOG_ERROR("{} failure(s)", failures);
        return 1;
    }

    spdlog::set_level(spdlog::level::info);
    SPDLOG_INFO("xpr_test_radio passed");
    return 0;
}
