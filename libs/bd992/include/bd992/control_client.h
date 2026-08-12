// SPDX-License-Identifier: GPL-3.0-or-later
//
// Request and reply over the control connection.
//
// A second socket, separate from the GSOF stream, and the separation buys two
// things. The framer on the stream side only ever sees GENOUT, so it needs no
// notion of correlating a reply with a request; and a configuration read --
// which can take a receiver a moment, and which happens while a service call
// waits -- cannot stall position output.
//
// Everything here is synchronous and serialised by a mutex, because it is
// called from zenoh service threads and a receiver answers one question at a
// time. The connection is opened lazily and reopened after a failure: the
// control socket is idle almost always, so holding it open through a receiver
// reboot buys nothing, and a service call that reconnects is indistinguishable
// from one that was slow.

#ifndef BD992_CONTROL_CLIENT_H
#define BD992_CONTROL_CLIENT_H

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "bd992/byte_stream.h"
#include "bd992/error.h"
#include "gsof/commands.h"
#include "gsof/framer.h"
#include "gsof/trimcomm.h"

namespace bd992
{

class ControlClient
{
  public:
    using StreamFactory = std::function<Result<std::unique_ptr<ByteStream>>()>;

    struct Options
    {
        // How long to wait for a reply before giving up. Generous: reading an
        // application file back makes the receiver assemble it.
        std::chrono::milliseconds replyTimeout { 3000 };

        // Which stored application file holds the running configuration. The
        // ICD only documents index 0 as the factory defaults, so this is a
        // setting rather than a constant and `--probe` exists to find it.
        std::uint16_t applicationFileIndex { 1 };

        // Refuse sendRaw(). Off by default: an arbitrary command can leave a
        // receiver unreachable, so reaching the rest of the ICD is opt-in.
        bool allowRawCommands { false };
    };

    struct Reply
    {
        std::uint8_t status { 0 };
        std::uint8_t type { 0 };
        std::vector<std::uint8_t> data;
    };

    ControlClient(StreamFactory factory, Options options);
    ~ControlClient();

    ControlClient(const ControlClient&) = delete;
    ControlClient& operator=(const ControlClient&) = delete;

    // Send a complete packet and wait for the first reply that is not a GSOF
    // report. NAK becomes Error::Kind::Refused rather than a Reply, because a
    // caller that forgot to check would otherwise treat a refusal as success.
    Result<Reply> exchange(std::span<const std::uint8_t> packet);

    // GETAPPFILE, reassembled. This is the read half of read-before-write.
    Result<gsof::appfile::ApplicationFile> readApplicationFile();
    Result<gsof::appfile::ApplicationFile> readApplicationFile(std::uint16_t index);

    // Send an application file built from `records`, echoing back the device
    // type the receiver last reported. Waits for the ACK.
    //
    // Passing no records is refused rather than sent: an empty application
    // file with the start flag set is a command to apply nothing, which is
    // pointless at best.
    Result<void> writeApplicationFile(std::span<const std::uint8_t> records);

    // GETOPT, returned as the raw reply payload -- the option list's layout is
    // not modelled, and reporting the bytes is more useful than reporting
    // nothing.
    Result<Reply> readOptions(std::uint8_t page = 0);

    // The escape hatch: any packet type, any payload. Gated by
    // Options::allowRawCommands.
    Result<Reply> sendRaw(std::uint8_t packetType, std::span<const std::uint8_t> data);

    // The device type the receiver reported in the last application file read.
    // Echoed back on write rather than guessed -- see gsof/commands.h.
    std::uint8_t deviceType() const;

    // Drop the connection. The next call reopens it.
    void disconnect();

  private:
    // Caller holds mMutex.
    Result<ByteStream*> ensureConnected();
    Result<Reply> exchangeLocked(std::span<const std::uint8_t> packet);
    // Read packets until `accept` is satisfied or the deadline passes.
    Result<void> readUntil(std::chrono::steady_clock::time_point deadline,
                           const std::function<bool(const gsof::trimcomm::PacketView&)>& accept);

    StreamFactory mFactory;
    Options mOptions;

    mutable std::mutex mMutex;
    std::unique_ptr<ByteStream> mStream;
    gsof::Framer mFramer;
    std::uint8_t mDeviceType { 0 };
    // Incremented per application file sent, as the ICD describes.
    std::uint8_t mTransmissionNumber { 0 };
};

} // namespace bd992

#endif // BD992_CONTROL_CLIENT_H
