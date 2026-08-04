// SPDX-License-Identifier: GPL-3.0-or-later
//
// An SDO client: the only way to read or write a device's object dictionary,
// and therefore the thing every part of the reconfiguration is built out of.
//
// Two properties matter more here than anywhere else in this library.
//
// The first is that every exchange is confirmed. The node this replaced sent a
// heartbeat-configuration write and never looked at the answer, so a device
// that aborted it looked exactly like a device that accepted it. Nothing in
// this class returns success without having seen a matching response.
//
// The second is that an upload reports the width the server actually served.
// That sounds like a detail; it is the entire question behind whether MoTeC's
// PDM Manager will talk to a keypad. Its SDO reader compares the response
// command byte for exact equality against the width it expects -- 0x4B for the
// two-byte 0x2010:02, 0x4F for the one-byte 0x1800:02 -- and reports a
// different error for a width mismatch than for a value mismatch. So "how many
// bytes came back" is diagnostic information, not an implementation detail to
// be normalised away into a uint32_t.
#ifndef CANOPEN_SDO_H
#define CANOPEN_SDO_H

#include "canopen/bus.h"

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace canopen
{

// The abort codes worth naming. CiA 301 defines many more; anything not listed
// is reported by its number.
enum class SdoAbortCode : uint32_t
{
    ToggleBitNotAlternated = 0x05030000,
    TimedOut = 0x05040000,
    CommandSpecifierInvalid = 0x05040001,
    UnsupportedAccess = 0x06010000,
    ReadOfWriteOnly = 0x06010001,
    WriteOfReadOnly = 0x06010002,
    ObjectDoesNotExist = 0x06020000,
    CannotBeMappedToPdo = 0x06040041,
    PdoLengthExceeded = 0x06040042,
    GeneralParameterIncompatibility = 0x06040043,
    GeneralInternalIncompatibility = 0x06040047,
    HardwareError = 0x06060000,
    LengthMismatch = 0x06070010,
    LengthTooHigh = 0x06070012,
    LengthTooLow = 0x06070013,
    SubIndexDoesNotExist = 0x06090011,
    ValueRangeExceeded = 0x06090030,
    ValueTooHigh = 0x06090031,
    ValueTooLow = 0x06090032,
    GeneralError = 0x08000000,
    CannotTransfer = 0x08000020,
    CannotTransferLocalControl = 0x08000021,
    CannotTransferDeviceState = 0x08000022,
    NoObjectDictionary = 0x08000023,
};

// Decodes the code, naming it where CiA 301 does. Never returns empty.
std::string describe_abort(uint32_t code);

struct SdoError
{
    enum class Kind
    {
        // No response arrived within the timeout. On a real bus this usually
        // means the wrong node ID, the wrong bit rate, or a device that is not
        // there at all.
        Timeout,
        // The device refused, and said why.
        Abort,
        // A response arrived that does not belong to the request: wrong
        // COB-ID, wrong index or sub-index echoed back, a command specifier
        // that makes no sense for what was asked.
        BadResponse,
    };

    Kind kind { Kind::Timeout };
    // Only meaningful for Abort.
    uint32_t abortCode { 0 };
    std::string message;
};

std::string to_string(const SdoError& error);

template <typename T>
using SdoResult = std::expected<T, SdoError>;

// The frames an exchange puts on the wire.
//
// Exposed so that a dry run prints exactly what an apply would send, from the
// same code that sends it. A tool whose preview is assembled separately from
// its action is a tool whose preview can be wrong about the one thing it is
// for.
helpers::CanFrame make_sdo_upload_frame(uint8_t nodeId, uint16_t index, uint8_t sub);
helpers::CanFrame make_sdo_download_frame(uint8_t nodeId, uint16_t index, uint8_t sub,
                                          std::span<const uint8_t> bytes);

// "12 34 56 78", for logs and dry runs.
std::string format_frame_data(const helpers::CanFrame& frame);

// What an upload returned, with the width preserved.
struct SdoData
{
    std::vector<uint8_t> bytes;
    // True when the server answered in a single expedited frame. A segmented
    // transfer says nothing about the object's declared width, so the
    // width-sensitive checks only apply to expedited results.
    bool expedited { true };

    size_t size() const { return bytes.size(); }

    // Little-endian, zero-extended. A value wider than the request is
    // truncated rather than silently reinterpreted.
    uint64_t as_uint() const;
};

class SdoClient
{
public:
    SdoClient(Bus& bus, uint8_t nodeId, Duration timeout = Duration { 1000 });
    ~SdoClient();

    SdoClient(const SdoClient&) = delete;
    SdoClient& operator=(const SdoClient&) = delete;

    // The node this client talks to. Changed by an LSS reconfiguration, at
    // which point every subsequent request goes to the new address.
    uint8_t node_id() const { return nodeId_; }
    void set_node_id(uint8_t nodeId) { nodeId_ = nodeId; }

    Duration timeout() const { return timeout_; }
    void set_timeout(Duration timeout) { timeout_ = timeout; }

    // --- upload (device -> us) ---------------------------------------------
    SdoResult<SdoData> upload(uint16_t index, uint8_t sub);
    SdoResult<uint8_t> upload_u8(uint16_t index, uint8_t sub);
    SdoResult<uint16_t> upload_u16(uint16_t index, uint8_t sub);
    SdoResult<uint32_t> upload_u32(uint16_t index, uint8_t sub);
    // For the VISIBLE_STRING objects (0x1008, 0x1009, 0x100A), which are the
    // only reason segmented transfer is implemented at all.
    SdoResult<std::string> upload_string(uint16_t index, uint8_t sub);

    // --- download (us -> device) -------------------------------------------
    SdoResult<void> download(uint16_t index, uint8_t sub, std::vector<uint8_t> bytes);
    SdoResult<void> download_u8(uint16_t index, uint8_t sub, uint8_t value);
    SdoResult<void> download_u16(uint16_t index, uint8_t sub, uint16_t value);
    SdoResult<void> download_u32(uint16_t index, uint8_t sub, uint32_t value);

    // 0x1010:sub <- "save", 0x1011:sub <- "load". The signatures are ASCII and
    // are checked by the device, so they are not parameters.
    SdoResult<void> store_parameters(uint8_t sub = 1);
    SdoResult<void> restore_parameters(uint8_t sub = 1);

    // Every exchange, in and out, for the log that turns a hardware session
    // into evidence. Called with a line already formatted.
    void on_exchange(std::function<void(const std::string&)> callback);

    static constexpr uint32_t kRequestCobIdBase = 0x600;
    static constexpr uint32_t kResponseCobIdBase = 0x580;

private:
    struct Response
    {
        helpers::CanFrame frame;
        bool present { false };
    };

    // Sends `request` and waits for the matching response, checking the
    // COB-ID, the echoed index and sub-index, and decoding an abort.
    SdoResult<helpers::CanFrame> exchange(const helpers::CanFrame& request, uint16_t index,
                                          uint8_t sub, bool checkEcho);

    SdoResult<SdoData> upload_segmented(uint16_t index, uint8_t sub, size_t declaredSize);

    void log(const std::string& line) const;
    void log_frame(const char* direction, const helpers::CanFrame& frame) const;

    Bus& bus_;
    uint8_t nodeId_;
    Duration timeout_;
    Response pending_;
    std::function<void(const std::string&)> exchangeCallback_;
};

} // namespace canopen

#endif // CANOPEN_SDO_H
