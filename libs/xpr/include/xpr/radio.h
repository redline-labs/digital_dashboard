// SPDX-License-Identifier: GPL-3.0-or-later
//
// An XNL session with a radio, and the typed queries on top of it.
//
// One socket carries everything: the handshake, every command, and the
// unsolicited broadcasts the radio pushes when its own state changes. So this
// class owns the socket, serialises access to it with a mutex, and is safe to
// call from a zenoh service thread while the node's own loop is pumping it.
//
// BROADCASTS ARE QUEUED, NOT DISPATCHED. They arrive interleaved with command
// replies, so a query has to read past them -- and dropping them there is how
// the display goes stale exactly when the channel changes, since a channel
// change is precisely when a query is in flight. Everything read while waiting
// for a reply is queued, and the node drains the queue on its own thread. That
// also keeps every publisher on one thread without a lock, which is what
// ZenohPublisher requires.
//
// THE TIMEOUT IS A DEADLINE, not a per-read budget. The obvious loop -- "read
// a frame, up to sixteen times, each with the full timeout" -- gives a radio
// that is chattering broadcasts a way to stall one query for sixteen times as
// long as the caller asked for, and a service call that hangs for eighty
// seconds looks like a dead radio rather than a busy one.

#ifndef XPR_RADIO_H
#define XPR_RADIO_H

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "mototrbo/control.h"
#include "mototrbo/xcmp.h"
#include "mototrbo/xnl.h"
#include "xpr/byte_stream.h"
#include "xpr/error.h"

namespace xpr
{

// An unsolicited 0xB4xx broadcast, owning its bytes so it can outlive the read
// buffer it was parsed from.
struct Broadcast
{
    std::uint16_t opcode { 0 };
    // From mototrbo::control::broadcast_name -- "display", "zone/channel",
    // "unknown" for one this build does not model.
    std::string name;
    std::vector<std::uint8_t> payload;

    struct DisplayLine
    {
        std::uint8_t line { 0 };
        std::uint8_t flags { 0 };
        // Decoded from UTF-16BE with the ANSI escapes stripped.
        std::string text;
    };

    std::optional<DisplayLine> display;
    std::optional<mototrbo::control::ZoneChannel> zoneChannel;
};

class Radio
{
  public:
    using StreamFactory = std::function<Result<std::unique_ptr<ByteStream>>()>;

    struct Options
    {
        // How long a query waits for its reply. The radio answers identity and
        // channel queries in milliseconds; this is generous so a busy radio is
        // not mistaken for an absent one.
        std::chrono::milliseconds replyTimeout { 2000 };

        // How long the handshake may take end to end. It is four round trips.
        std::chrono::milliseconds handshakeTimeout { 4000 };

        // Tried in order, then the last repeats. Nothing here sleeps: a failed
        // attempt sets the time of the next one and returns, so a node loop
        // stays responsive while the radio is unplugged.
        std::vector<std::chrono::milliseconds> reconnectBackoff {
            std::chrono::milliseconds(250), std::chrono::milliseconds(500),
            std::chrono::milliseconds(1000), std::chrono::milliseconds(2000),
            std::chrono::milliseconds(5000)
        };

        // The queue is bounded because a radio in a state that makes it
        // chatter must not grow this without limit if the node stops draining.
        // Oldest is dropped first and the drop is counted -- a display line
        // from ten seconds ago is worth less than the current one.
        std::size_t maxQueuedBroadcasts { 64 };
    };

    struct Stats
    {
        bool connected { false };
        std::uint16_t address { 0 };

        std::uint64_t connects { 0 };
        std::uint64_t connectFailures { 0 };
        std::uint64_t disconnects { 0 };

        std::uint64_t commands { 0 };
        std::uint64_t replies { 0 };
        std::uint64_t broadcasts { 0 };
        std::uint64_t broadcastsDropped { 0 };
        // Frames read while waiting for a reply that were neither the reply
        // nor a broadcast: acknowledgements, and replies to a query that had
        // already timed out. A climbing count means queries are overlapping.
        std::uint64_t framesSkipped { 0 };
        std::uint64_t decodeErrors { 0 };

        // Empty unless something failed. The first thing to look at when no
        // topics appear.
        std::string lastError;
    };

    Radio(StreamFactory factory, Options options);
    ~Radio();

    Radio(const Radio&) = delete;
    Radio& operator=(const Radio&) = delete;

    // Connect and complete the XNL handshake now, ignoring the backoff
    // schedule. Idempotent while connected.
    Result<void> connect();

    void disconnect();

    bool connected() const;

    Stats stats() const;

    // Send a command and return the reply BODY -- what follows the result
    // code, with a non-zero result code turned into Error::Kind::Refused.
    //
    // Fails at once with NotConnected if the session is down. It does not
    // open one: connecting costs the connect timeout and holds the lock, so a
    // query arriving while the radio is unplugged would stall the node for
    // whoever asked. Reconnection is pump()'s job.
    // Everything below is built on this; it is public because reaching a
    // read-only opcode this build does not wrap should not require editing the
    // library.
    Result<std::vector<std::uint8_t>> transact(const mototrbo::xcmp::Command& command);

    // ---- what the radio is doing ------------------------------------------

    Result<mototrbo::control::ZoneChannel> channel();

    struct ChannelCounts
    {
        std::uint16_t zones { 0 };
        // Always for the zone the radio is currently on: the count query's
        // argument is ignored by the radio.
        std::uint16_t channelsInZone { 0 };
    };

    Result<ChannelCounts> channelCounts();

    struct StatusValue
    {
        mototrbo::control::StatusItem item {};
        mototrbo::control::StatusEncoding encoding { mototrbo::control::StatusEncoding::Opaque };

        // Set according to the item's encoding. `raw` is always the bytes the
        // radio sent, so an item whose meaning we do not know is still
        // reportable.
        std::string text;
        std::uint32_t number { 0 };
        bool hasNumber { false };
        std::vector<std::uint8_t> raw;
    };

    // An item the table marks unsupported is refused locally rather than
    // asked: a mobile answers a station meter with a result code, and turning
    // that into a protocol error every second would be noise.
    Result<StatusValue> status(mototrbo::control::StatusItem item);

    struct Identity
    {
        std::string modelNumber;
        std::string serialNumber;
        std::string firmwareVersion;
        std::string tanapaNumber;

        // The radio's DMR id. It appears in no readable codeplug item under
        // any encoding, so this query is the only way to learn it.
        std::uint32_t radioId { 0 };
        bool radioIdKnown { false };

        // Manufacture datecode, undecoded: seven bytes whose field layout is
        // not documented anywhere we have.
        std::vector<std::uint8_t> datecode;
    };

    // Best effort: a field the radio refuses is left empty rather than failing
    // the call, because which items a radio answers varies by model and a
    // half-known identity is more useful than none. Fails only if the session
    // itself is unusable.
    Result<Identity> identity();

    // ---- changing the channel ---------------------------------------------

    // Step, and return where the radio ended up. This is the ONLY way to move
    // an XPR 5550: the direct-select operation is accepted and inert, see
    // mototrbo::control::select_zone_channel.
    Result<mototrbo::control::ZoneChannel> stepChannel(bool up);

    // Step until the radio reports `channel`, or give up.
    //
    // Implemented by stepping because direct select does not move this radio.
    // Two guards, both of which have to exist because the radio reports
    // success either way: a full cycle of the zone's channel count without
    // arriving means the channel is not reachable, and a step that does not
    // change the reported channel means the radio has stopped accepting them.
    //
    // A zone other than the one the radio is on is REFUSED rather than
    // attempted: zone cannot be changed over XNL on this radio at all, and
    // stepping channels in the hope of crossing a zone boundary would be a
    // guess with the user's radio.
    Result<mototrbo::control::ZoneChannel> selectChannel(std::uint16_t zone, std::uint16_t channel);

    // ---- broadcasts --------------------------------------------------------

    // Read for up to `timeout`, queueing whatever broadcasts arrive, and keep
    // the session up: this is where a disconnected radio is reconnected, on
    // the backoff schedule. Call it from the node's loop.
    void pump(std::chrono::milliseconds timeout);

    // Hand over everything queued since the last call.
    std::vector<Broadcast> takeBroadcasts();

  private:
    // All of these expect mMutex to be held.
    Result<void> ensureConnectedLocked(bool force);
    Result<void> handshakeLocked();
    Result<void> sendFrameLocked(mototrbo::xnl::Opcode opcode, std::uint8_t protocol, std::uint8_t flags,
                                 std::uint16_t dst, std::uint16_t src, std::uint16_t transaction,
                                 std::span<const std::uint8_t> payload);
    // The returned frame's payload points into mFrame and stays valid until
    // the next call.
    Result<mototrbo::xnl::Frame> readFrameLocked(std::chrono::steady_clock::time_point deadline);
    Result<mototrbo::xnl::Frame> readFrameOfLocked(mototrbo::xnl::Opcode wanted,
                                                   std::chrono::steady_clock::time_point deadline);
    Result<std::vector<std::uint8_t>> transactLocked(const mototrbo::xcmp::Command& command);
    void pumpLocked(std::chrono::steady_clock::time_point deadline);
    // Returns true if the frame was a broadcast and was queued.
    bool queueBroadcastLocked(const mototrbo::xnl::Frame& frame);
    void dropConnectionLocked(std::string reason);
    std::uint8_t nextFlagsLocked();
    std::uint16_t nextTransactionLocked();

    StreamFactory mFactory;
    Options mOptions;

    mutable std::mutex mMutex;
    std::unique_ptr<ByteStream> mStream;

    // Bytes read from the socket that do not yet make a whole frame.
    std::vector<std::uint8_t> mRxBuffer;
    // The frame currently being handed to a caller, moved out of mRxBuffer so
    // that consuming the buffer cannot invalidate the payload span.
    std::vector<std::uint8_t> mFrame;

    std::deque<Broadcast> mBroadcasts;

    bool mConnected { false };
    std::uint16_t mAddress { 0 };
    std::uint16_t mMasterAddress { 0 };

    // Rolling 0..7, stamped into every data message's flags byte. THE RADIO
    // DEDUPES ON IT: if it does not advance, every message after the first is
    // discarded as a retransmission and the session appears to hang.
    std::uint8_t mFlags { 0 };
    std::uint16_t mTransaction { 0 };

    // Set when the last pump found no session, so the caller's budget is spent
    // sleeping rather than spinning. Only ever touched under the lock.
    bool mPumpIdle { false };

    std::size_t mBackoffIndex { 0 };
    std::chrono::steady_clock::time_point mNextConnectAttempt {};

    Stats mStats;
};

} // namespace xpr

#endif // XPR_RADIO_H
