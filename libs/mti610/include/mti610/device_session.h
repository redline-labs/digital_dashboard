// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Config/Measurement handshake, as a synchronous helper over a ByteStream.
//
// THIS IS THE PIECE THE BD992 HAS NO EQUIVALENT OF. A Trimble receiver has no
// modes: it streams, and configuration happens on a second socket while it
// keeps streaming. An MTi has exactly two states and one port. It answers
// configuration messages only in Config state, and it emits MTData2 only in
// Measurement state, so reconfiguring means stopping the data, and reading the
// data means giving up the ability to ask anything.
//
// Everything here is synchronous and single-threaded on purpose. The threading
// lives one level up in StreamClient, which owns the port and calls into this
// between reads -- so a scripted peer on a pty can drive the entire handshake
// with no thread and no device. A session that spawned its own thread would
// need a device to test.
//
// Two hazards shape exchange():
//
//   * A REPLY IS NOT THE NEXT MESSAGE. GoToConfig arrives while the device is
//     mid-measurement, so the bytes after it are the tail of an MTData2 and
//     then however many more the device had already queued. The exchange has
//     to read past them, which is why it takes the framer rather than raw
//     bytes.
//
//   * AN UNSOLICITED WakeUp CAN ARRIVE AT ANY TIME. It means the device reset
//     -- a brown-out, a watchdog, somebody's power cycle -- and it must be
//     answered within 500 ms or the device enters Measurement with its STORED
//     configuration, which may not be the one we asked for. A node that missed
//     this would keep publishing, with the wrong outputs at the wrong rates,
//     and nothing anywhere would say so.

#ifndef MTI610_DEVICE_SESSION_H
#define MTI610_DEVICE_SESSION_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "mti610/byte_stream.h"
#include "mti610/error.h"
#include "mti610/output_config.h"
#include "xbus/commands.h"
#include "xbus/framer.h"

namespace mti610
{

// What the device says about itself. Filled by identify(); a field the device
// declined to answer for is left at its default rather than faked.
struct DeviceInfo
{
    std::uint64_t deviceId { 0 };
    std::string productCode;
    std::uint8_t firmwareMajor { 0 };
    std::uint8_t firmwareMinor { 0 };
    std::uint8_t firmwareRevision { 0 };
    std::uint32_t firmwareBuild { 0 };
    std::uint8_t hardwareMajor { 0 };
    std::uint8_t hardwareMinor { 0 };

    std::string firmwareVersion() const;

    // Whether the product code looks like the device this library models.
    //
    // Deliberately a report rather than a refusal. A 620 or a 630 speaks the
    // same protocol and its extra outputs land on the raw topic; refusing to
    // talk to one would be worse than saying so and carrying on. See
    // docs/nodes/mti610_bridge.md.
    bool looksLikeMti610() const;
};

struct SessionOptions
{
    // How long to wait for an acknowledgement. The LLCP gives no figure; a
    // second is far longer than a device on a healthy link takes and short
    // enough that a wrong baud rate is obvious rather than a hang.
    unsigned replyTimeoutMs { 1000 };

    // How many times to resend a command that was not answered. A serial link
    // drops bytes; one retry costs nothing and turns a transient into a
    // non-event.
    unsigned retries { 2 };

    ConfigMode mode { ConfigMode::Enforce };
    PortPolicy policy { PortPolicy::Additive };
};

// What configure() did, for the node's status message and for --check.
struct ConfigureResult
{
    std::vector<Change> changes;
    bool wrote { false };

    // What the device reports it is actually sending, after any write. These
    // are the device's own effective rates, which may be lower than what was
    // asked for -- it clamps rather than refusing.
    std::vector<OutputEntry> effective;
};

class DeviceSession
{
  public:
    // Neither the stream nor the framer is owned. Both belong to StreamClient,
    // which is what makes an exchange able to read past the data messages that
    // arrive in the middle of one.
    DeviceSession(ByteStream& stream, xbus::Framer& framer, SessionOptions options);

    // Called with any MTData2 that turns up during a configuration exchange.
    // Optional: during startup there is nothing useful to do with one, but
    // during a mid-stream reconfigure dropping them silently would look like a
    // gap in the data.
    using DataHandler = std::function<void(const xbus::MessageView&)>;
    void onData(DataHandler handler) { mOnData = std::move(handler); }

    // Called when the device reports a Warning (MID 0x43). Warnings are not
    // failures and are not worth failing an exchange over, but they are worth
    // seeing.
    using WarningHandler = std::function<void(std::uint32_t code, const std::string& text)>;
    void onWarning(WarningHandler handler) { mOnWarning = std::move(handler); }

    // Send one message and read until its acknowledgement arrives.
    //
    // Handles the three things that can come back instead: an Error message
    // becomes Kind::Refused, a Warning is reported and ignored, an MTData2 is
    // handed to onData(). An unsolicited WakeUp is answered immediately and
    // the exchange is retried from the top, because the device has just
    // forgotten whatever state it was in.
    Result<xbus::MessageView> exchange(std::span<const std::uint8_t> command,
                                       xbus::MessageId expectedAck);

    // Enter Config state. Valid from either state, so this doubles as "confirm
    // we are in Config".
    Result<void> goToConfig();

    // Enter Measurement state and start the data flowing.
    Result<void> goToMeasurement();

    // Answer a WakeUp. Sent without waiting for anything, because WakeUpAck's
    // acknowledgement is the device entering Config rather than a message.
    bool sendWakeUpAck();

    Result<DeviceInfo> identify();

    Result<std::vector<OutputEntry>> readOutputConfig();

    // Write the list and return what the device echoes back, which is the
    // authoritative answer about what it will actually send.
    Result<std::vector<OutputEntry>> writeOutputConfig(const std::vector<OutputEntry>& entries);

    // The whole read-compare-write pass. Assumes Config state has already been
    // entered.
    Result<ConfigureResult> configure(const std::vector<OutputEntry>& desired);

  private:
    // One attempt at exchange(), without the retry loop.
    Result<xbus::MessageView> exchangeOnce(std::span<const std::uint8_t> command,
                                           xbus::MessageId expectedAck, bool& sawWakeUp);

    // Pump bytes from the stream into the framer until `deadline`.
    bool pump(unsigned timeoutMs);

    ByteStream& mStream;
    xbus::Framer& mFramer;
    SessionOptions mOptions;

    DataHandler mOnData;
    WarningHandler mOnWarning;
};

} // namespace mti610

#endif // MTI610_DEVICE_SESSION_H
