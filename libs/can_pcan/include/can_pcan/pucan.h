// SPDX-License-Identifier: GPL-3.0-or-later
//
// The uCAN protocol the PCAN-USB FD family speaks, as a pure codec.
//
// PEAK's FD-generation adapters -- PCAN-USB FD (0x0012), PCAN-USB Pro FD
// (0x0011), PCAN-USB X6 (0x0014) -- do not speak the older register-poking
// protocol of the classic PCAN-USB. They speak "uCAN": a small command set
// written into a buffer and pushed over a bulk endpoint, and a stream of
// length-prefixed records coming back the other way. Both directions carry a
// channel index in every record, which is how one USB handle serves up to six
// CAN channels.
//
// PROVENANCE: the structures and opcodes below follow the uCAN definitions
// implemented by the mainline Linux `peak_usb` driver
// (drivers/net/can/usb/peak_usb/pcan_usb_fd.c and
// include/linux/can/dev/peak_canfd.h), which is the openly documented
// description of this protocol.
//
// They have now been confirmed against a PCAN-USB Pro FD (firmware 3.4.4),
// and the warning that used to be here -- that the numbers were the thing to
// check first if a real dongle did not answer -- turned out to be exactly
// right. A real dongle did not answer, and four things were wrong:
//
//   Bit timing was encoded without the "counted from zero" decrement on every
//   field except the prescaler, which asked for 952 kbit/s when 1 Mbit/s was
//   configured. 4.8% off is far outside what CAN tolerates, so the channel
//   transmitted nothing, received nothing, and reported no error at all.
//
//   The timing field masks were narrower than the device's, so a long tseg1
//   was truncated into a different bit rate at the slower bus speeds.
//
//   The standard acceptance filter was written as one command when it is 64
//   rows of 32 identifiers, so only identifiers 0x000..0x01F could arrive.
//
//   PUCAN_OPTION_ERROR was never enabled, so the device sent no error records
//   and the error counters, bus state and bus-off count could not move.
//
// The lesson worth keeping: every one of those failed SILENTLY. There is no
// error path in this protocol for "your timing is wrong" -- the controller
// simply never wins arbitration, and a bus with a working cable, a working
// dongle and a working driver carries nothing.
//
// Everything in this header is free of libusb and of any I/O. That is the point:
// the parts that are easy to get wrong -- the DLC coding, the flag bits, the
// bit-timing register packing, walking a buffer of variable-length records --
// are all exercised by tests on a machine with no CAN hardware.
#ifndef CAN_PCAN_PUCAN_H
#define CAN_PCAN_PUCAN_H

#include "can/bitrate.h"
#include "can/channel.h"
#include "can/error.h"

#include "helpers/can_frame.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace can::pcan
{

// --- USB identity ----------------------------------------------------------

inline constexpr uint16_t kPeakVendorId = 0x0c72;

inline constexpr uint16_t kProductUsbFd = 0x0012;   // PCAN-USB FD, 1 channel
inline constexpr uint16_t kProductUsbProFd = 0x0011; // PCAN-USB Pro FD, 2 channels
inline constexpr uint16_t kProductUsbX6 = 0x0014;   // PCAN-USB X6, 6 channels
inline constexpr uint16_t kProductUsbChip = 0x0013; // PCAN-Chip USB, 1 channel

// How many CAN channels a product exposes, or 0 if this is not a device we
// know how to drive.
uint8_t channel_count_for_product(uint16_t productId);
const char* product_name(uint16_t productId);

// The FD family runs its CAN controllers from an 80 MHz clock.
inline constexpr uint32_t kControllerClockHz = 80000000;

// What the controller can generate, for the bit-timing solver. The data phase
// has a much narrower range than the nominal phase, which is why FD needs the
// calculation done twice rather than scaled.
BitTimingLimits nominal_bit_timing_limits();
BitTimingLimits data_bit_timing_limits();

// --- endpoints -------------------------------------------------------------
//
// Defaults for the family. A device can report its own in the firmware info
// block (the "extended" form), and when it does those win -- the X6 in
// particular does not use the same layout as a two-channel Pro FD.

inline constexpr uint8_t kDefaultCommandOutEndpoint = 0x01;
inline constexpr uint8_t kDefaultCommandInEndpoint = 0x81;
inline constexpr uint8_t kDefaultDataOutEndpoint = 0x02;
inline constexpr uint8_t kDefaultDataInEndpoint = 0x82;

// --- vendor control requests -----------------------------------------------

inline constexpr uint8_t kRequestInfo = 0;
inline constexpr uint8_t kRequestFunction = 2;

// wValue for kRequestInfo.
inline constexpr uint16_t kInfoBootloader = 0;
inline constexpr uint16_t kInfoFirmware = 1;

// wValue for kRequestFunction. "Driver loaded" is what takes the device out of
// its idle state; without it the adapter answers nothing on the bulk endpoints.
inline constexpr uint16_t kFunctionDriverLoaded = 5;
inline constexpr size_t kFunctionDriverLoadedLength = 16;

std::vector<uint8_t> encode_driver_loaded(bool loaded);

// --- firmware info ---------------------------------------------------------

// What the device says about itself, decoded from the kRequestInfo/
// kInfoFirmware control transfer.
struct FirmwareInfo
{
    uint16_t structureSize { 0 };
    // 1 = the base record, 2 = the extended record that also carries endpoint
    // numbers. Only the extended form tells us which endpoints to use.
    uint16_t recordType { 0 };
    uint8_t hardwareType { 0 };
    uint8_t bootloaderVersion[3] { 0, 0, 0 };
    uint8_t hardwareVersion { 0 };
    uint8_t firmwareVersion[3] { 0, 0, 0 };
    uint32_t deviceId[2] { 0, 0 };
    uint32_t serialNumber { 0 };
    uint32_t flags { 0 };

    // Only meaningful when recordType is the extended form.
    bool hasEndpoints { false };
    uint8_t commandOutEndpoint { kDefaultCommandOutEndpoint };
    uint8_t commandInEndpoint { kDefaultCommandInEndpoint };
    uint8_t dataOutEndpoint[2] { kDefaultDataOutEndpoint, 0x03 };
    uint8_t dataInEndpoint { kDefaultDataInEndpoint };

    std::string firmwareVersionString() const;
};

inline constexpr size_t kFirmwareInfoMinLength = 36;

Result<FirmwareInfo> decode_firmware_info(std::span<const uint8_t> bytes);

// --- commands --------------------------------------------------------------
//
// A command is eight bytes: a little-endian opcode-and-channel word followed by
// six bytes of argument. Several are packed into one buffer and pushed in a
// single transfer, terminated by an end-of-collection marker.

enum class Opcode : uint16_t
{
    Nop = 0x000,
    ResetMode = 0x001,
    NormalMode = 0x002,
    ListenOnlyMode = 0x003,
    TimingSlow = 0x004,
    TimingFast = 0x005,
    SetStdFilter = 0x006,
    FilterStd = 0x008,
    TxAbort = 0x009,
    WriteErrorCounters = 0x00a,
    SetEnableOption = 0x00b,
    ClearDisableOption = 0x00c,
    // 0x00d is reserved; the barrier is 0x010. Getting this wrong sends a
    // command the device does not implement, which it ignores in silence.
    RxBarrier = 0x010,

    // Extensions specific to the PCAN-USB FD family.
    ClockSet = 0x080,
    DeviceIdSet = 0x081,
    LedSet = 0x086,

    EndOfCollection = 0x3ff,
};

inline constexpr size_t kCommandSize = 8;
// The command buffer the device expects on a full-speed connection. Commands
// are padded out to this before being sent.
inline constexpr size_t kCommandBufferSize = 512;
inline constexpr size_t kLowSpeedCommandBufferSize = 64;

// Packs the opcode into the low ten bits and the channel into bits 12..15,
// which is how a command reaches one CAN controller of a six-channel device.
uint16_t opcode_channel(uint8_t channel, Opcode opcode);

// Builds one command into a buffer. `args` is padded or truncated to six bytes.
void append_command(std::vector<uint8_t>& buffer, uint8_t channel, Opcode opcode,
                    std::span<const uint8_t> args = {});

// Bit timing, packed as the device wants it.
//
// The two commands are not the same shape: the nominal phase carries an error
// warning limit and a triple-sampling flag that the data phase has no room
// for, and their field widths differ because the data phase's segments are
// smaller. Encoding them with one function and a flag would hide that.
void append_timing_slow(std::vector<uint8_t>& buffer, uint8_t channel, const BitTiming& timing,
                        uint8_t errorWarningLimit = 96, bool tripleSampling = false);
void append_timing_fast(std::vector<uint8_t>& buffer, uint8_t channel, const BitTiming& timing);

void append_clock(std::vector<uint8_t>& buffer, uint8_t channel, uint8_t clockSelector);
void append_options(std::vector<uint8_t>& buffer, uint8_t channel, bool enable, uint16_t ucanMask,
                    uint16_t usbMask);
void append_std_filter_pass_all(std::vector<uint8_t>& buffer, uint8_t channel);
void append_led(std::vector<uint8_t>& buffer, uint8_t channel, uint8_t mode);

// Pads a built-up command buffer to the size the device expects.
void finish_command_buffer(std::vector<uint8_t>& buffer, size_t bufferSize = kCommandBufferSize);

// The clock selector for ClockSet. 80 MHz is the default and the only one the
// bit-timing limits above describe.
inline constexpr uint8_t kClock80MHz = 0;
inline constexpr uint8_t kClock60MHz = 1;
inline constexpr uint8_t kClock40MHz = 2;
inline constexpr uint8_t kClock30MHz = 3;
inline constexpr uint8_t kClock24MHz = 4;
inline constexpr uint8_t kClock20MHz = 5;

// Option bits for SetEnableOption / ClearDisableOption. The calibration
// messages are the device's own timestamp synchronisation traffic; leaving
// them on means a stream of records nothing wants.
inline constexpr uint16_t kOptionCalibrationMessages = 0x8000;

// uCAN option bits, which go in the OTHER mask of the same command. Error
// reporting is off until it is asked for: without this the device sends no
// error records at all, so the error counters and every bus state above
// ErrorActive stay at their initial values no matter what the bus does.
inline constexpr uint16_t kOptionError = 0x0001;
inline constexpr uint16_t kOptionBusLoad = 0x0002;
inline constexpr uint16_t kOptionCanFdNonIso = 0x0004;

// The 11-bit acceptance filter is 64 rows of 32 identifiers. "Pass everything"
// means writing every row -- one row with an all-ones mask passes identifiers
// 0..31 and silently drops the other 2016.
inline constexpr uint16_t kStdFilterRowCount = 64;

// --- received records ------------------------------------------------------

enum class RecordType : uint16_t
{
    CanRx = 0x0001,
    Error = 0x0002,
    Status = 0x0003,
    BusLoad = 0x0004,
    CacheCritical = 0x0102,
    CanTx = 0x1000,

    // PCAN-USB FD family additions.
    Calibration = 0x0100,
    Overrun = 0x0101,
};

// Message flags, shared by transmitted and received frames.
inline constexpr uint16_t kFlagRtr = 0x0001;
inline constexpr uint16_t kFlagExtendedId = 0x0002;
inline constexpr uint16_t kFlagLoopedBack = 0x0004;
inline constexpr uint16_t kFlagSingleShot = 0x0008;
inline constexpr uint16_t kFlagExtendedDataLength = 0x0010; // FD frame
inline constexpr uint16_t kFlagBitrateSwitch = 0x0020;      // FD BRS
inline constexpr uint16_t kFlagErrorStateIndicator = 0x0040; // FD ESI
inline constexpr uint16_t kFlagSelfReceive = 0x0080;

// Status record bits.
inline constexpr uint8_t kStatusErrorPassive = 0x20;
inline constexpr uint8_t kStatusErrorWarning = 0x40;
inline constexpr uint8_t kStatusBusOff = 0x80;

// One decoded record. Records that are not frames still carry a channel, so a
// multi-channel device's status and error reports go to the right place.
struct Record
{
    RecordType type { RecordType::CanRx };
    uint8_t channel { 0 };
    // Device timestamp in microseconds, from the record's 64-bit counter.
    uint64_t timestampUs { 0 };

    // Valid when type is CanRx or CanTx.
    helpers::CanFrame frame;

    // Valid when type is Status.
    BusState state { BusState::Unknown };

    // Valid when type is Error.
    uint8_t rxErrorCounter { 0 };
    uint8_t txErrorCounter { 0 };

    // Valid when type is Overrun: the device dropped frames.
    bool overrun { false };
};

// Walks a received bulk transfer, which holds however many whole records fit.
//
// A malformed record stops the walk rather than being skipped: the length field
// is what tells us where the next record starts, so once it is wrong there is
// no way to resynchronise inside the buffer, and guessing would turn one bad
// transfer into a stream of plausible nonsense.
struct DecodeResult
{
    std::vector<Record> records;
    // Set when the walk stopped early. Everything before it is still valid.
    std::optional<Error> error;
};

DecodeResult decode_records(std::span<const uint8_t> buffer);

// --- transmitted frames ----------------------------------------------------

// Encodes a frame as a CanTx record, appended to `buffer`. Fails when the
// frame cannot be represented: an identifier too wide for its format, an FD
// payload length that is not one of the representable sizes, or an FD frame on
// a channel that does not support it.
Result<void> append_tx_frame(std::vector<uint8_t>& buffer, uint8_t channel,
                             const helpers::CanFrame& frame, bool fdEnabled);

} // namespace can::pcan

#endif // CAN_PCAN_PUCAN_H
