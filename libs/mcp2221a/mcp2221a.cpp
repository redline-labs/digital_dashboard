#include "mcp2221a/mcp2221a.h"

#include <spdlog/spdlog.h>
#include <spdlog/fmt/ranges.h> // Required for fmt::join

#include <chrono>
#include <optional>
#include <thread>
#include <vector>
#include <cstring>

class Report_t
{
  public:
    static constexpr size_t kDataSize = 64u + 1u;  // One extra byte for the report ID.

    using payload_t = std::array<uint8_t, kDataSize>;

    constexpr Report_t(MCP2221ACommands cmd) :
        data_{}
    {
        data_[0] = 0x00;  // Report ID for the HID library, always zero.
        data_[1] = static_cast<uint8_t>(cmd);
    }

    constexpr Report_t(uint8_t cmd) :
        data_{}
    {
        data_[0] = 0x00;  // Report ID for the HID library, always zero.
        data_[1] = cmd;
    }

    constexpr uint8_t& operator[](size_t idx)
    {
        // TODO: Bounds railing.
        return data_[idx + 1u];  // Offset by one since we have the HID report ID.
    }

    constexpr const uint8_t& operator[](size_t idx) const
    {
        // TODO: Bounds railing.
        return data_[idx + 1u];  // Offset by one since we have the HID report ID.
    }

    constexpr operator uint8_t*()
    {
        return &data_[0];
    }

    constexpr operator const uint8_t*() const
    {
        return &data_[0];
    }

    constexpr size_t size() const
    {
        return kDataSize;
    }

  private:
    payload_t data_;
};

// One named builder per command, with the byte layout written out.
//
// These used to be explicit specialisations of a variadic make_report<Cmd>().
// That was quietly broken for the entire life of the driver: a specialisation
// only matches when the deduced argument types match *exactly*, and every call
// site passed promoted ints -- make_report<I2CWriteData>(0, addr << 1) deduces
// <int, int>, not <uint16_t, uint8_t>. So the specialisation was skipped, the
// primary template ran, and it packed the arguments consecutively from byte 1:
// the address landed in the length-MSB field and the address field stayed 0x00.
// Every I2C transfer this driver ever issued went to the general-call address,
// which is why nothing ever answered.
//
// Named functions with concrete parameter types cannot fail that way: a
// mismatched argument is a conversion, not a silent change of overload.
//
// Byte layouts are from DS20005565E sections 3.1.1, 3.1.7, 3.1.8 and 3.1.10.
// Note the report indices below are *command* byte indices; Report_t hides the
// leading HID report ID.

Report_t makeStatusSetParameters(bool cancel_i2c, uint32_t speed_hz)
{
    Report_t report(MCP2221ACommands::StatusSetParameters);
    report[1] = 0x00;                                 // don't care
    report[2] = cancel_i2c ? 0x10 : 0x00;             // cancel current transfer
    const bool set_speed = speed_hz > 1000u;
    report[3] = set_speed ? 0x20 : 0x00;              // set communication speed
    // The divider is only consumed when byte 3 requests a speed change, so
    // guard the division: callers that only cancel (or only read status) pass
    // speed_hz == 0, and dividing by that is undefined behaviour -- it traps on
    // x86 and silently yields garbage on AArch64.
    report[4] = set_speed ? static_cast<uint8_t>((12000000u / speed_hz) - 3u) : 0x00;
    return report;
}

Report_t makeReset()
{
    Report_t report(MCP2221ACommands::Reset);
    report[1] = 0xAB;
    report[2] = 0xCD;
    report[3] = 0xEF;
    return report;
}

// `address8` is the 8-bit form: 7-bit address shifted left, low bit 0 to write
// and 1 to read (DS20005565E 3.1.7 note 1).
Report_t makeI2cWrite(uint16_t length, uint8_t address8)
{
    Report_t report(MCP2221ACommands::I2CWriteData);
    report[1] = static_cast<uint8_t>(length & 0xFFu);         // length LSB
    report[2] = static_cast<uint8_t>((length >> 8) & 0xFFu);  // length MSB
    report[3] = address8;
    return report;
}

Report_t makeI2cRead(uint16_t length, uint8_t address8)
{
    Report_t report(MCP2221ACommands::I2CReadData);
    report[1] = static_cast<uint8_t>(length & 0xFFu);
    report[2] = static_cast<uint8_t>((length >> 8) & 0xFFu);
    report[3] = address8;
    return report;
}

Report_t makeGetI2cData()
{
    return Report_t(MCP2221ACommands::I2CGetData);
}


MCP2221A::MCP2221A() :
    device_{nullptr, hid_close}
{
    if (hid_init())
    {
        SPDLOG_ERROR("Failed to initialize HIDAPI");
    }
}

MCP2221A::~MCP2221A()
{
    hid_exit();
}

bool MCP2221A::open()
{
    device_ = {hid_open(kVendorId, kProductId, nullptr), hid_close};
    if (!device_)
    {
#ifdef __linux__
        SPDLOG_ERROR("MCP2221A device not found. If it is present in lsusb, check that a "
                     "/dev/hidraw node exists for it and is readable -- the in-kernel "
                     "hid_mcp2221 driver claims the chip without ever creating one, so it "
                     "has to be blacklisted.");
#else
        // None of that advice applies here: there is no hidraw node and no
        // hid_mcp2221 to blacklist, because macOS ships no kernel driver for
        // this chip -- which is the whole reason this HID backend exists.
        SPDLOG_ERROR("MCP2221A device not found. Check it is plugged in and appears in "
                     "System Information > USB as vendor 0x04d8 product 0x00dd.");
#endif
        return false;
    }

    // A pure status query: no cancel, no speed change. It proves the handle is
    // backed by a live device (a successful hid_open() alone does not, since it
    // can land on a node that is about to disappear) and tells us whether the
    // I2C engine is usable.
    const auto status = get_status();
    if (!status)
    {
        SPDLOG_ERROR("MCP2221A did not answer a status query after open");
        device_.reset();
        return false;
    }

    if (status->i2c_state == I2CState::Idle)
    {
        return true;
    }

    // The engine is latched in a non-idle state -- typically StopTimeout (0x62)
    // left behind by an earlier run, since nothing power-cycles the chip
    // between processes. Every parameter change is refused while it persists.
    //
    // Only a device Reset clears this. Cancel does not: verified on hardware,
    // five consecutive cancels were acknowledged and left the state at 0x62.
    // Reset costs a full USB re-enumeration, so it is deliberately conditional
    // on the engine actually being wedged rather than run on every open.
    SPDLOG_WARN("MCP2221A I2C engine is in state 0x{:02x}, not idle; resetting the device",
                static_cast<uint8_t>(status->i2c_state));
    return reset_and_reopen();
}

bool MCP2221A::reset_and_reopen()
{
    auto report = makeReset();
    if (hid_write(device_.get(), report, report.size()) == -1)
    {
        SPDLOG_ERROR("Failed to send reset command");
        return false;
    }

    // The handle dies with the old USB device.
    device_.reset();

    // Re-enumeration means: the kernel re-adds the device, binds hid-generic,
    // creates a fresh hidraw node, and udev applies its ownership rule to it.
    // The node path is recycled, so it cannot be used to tell the new device
    // from the old one -- instead every candidate handle has to prove itself
    // with a real transaction, which also rejects a handle opened against the
    // old node in the window before it disappears.
    //
    // Measured at ~6 s through VMware USB passthrough, so the budget is
    // generous; bare metal is far quicker and exits the loop early.
    constexpr auto kPollInterval = std::chrono::milliseconds(100);
    constexpr auto kTimeout = std::chrono::seconds(20);

    const auto deadline = std::chrono::steady_clock::now() + kTimeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(kPollInterval);

        decltype(device_) candidate{hid_open(kVendorId, kProductId, nullptr), hid_close};
        if (!candidate)
        {
            continue;
        }

        device_ = std::move(candidate);
        if (const auto status = get_status(); status && status->i2c_state == I2CState::Idle)
        {
            SPDLOG_INFO("MCP2221A reset complete; I2C engine idle");
            return true;
        }
        device_.reset();
    }

    SPDLOG_ERROR("MCP2221A did not come back in a usable state after reset. If it is present "
                 "in lsusb, check that a /dev/hidraw node exists for it and is readable -- "
                 "the in-kernel hid_mcp2221 driver claims the chip without ever creating "
                 "one, so it has to be blacklisted.");
    return false;
}

bool MCP2221A::is_open() const
{
    return device_ != nullptr;
}

std::optional<MCP2221AStatus> MCP2221A::get_status_set_parameters(bool cancel_i2c, uint32_t speed_hz)
{
    if (!is_open())
    {
        return std::nullopt;
    }

    auto report = makeStatusSetParameters(cancel_i2c, speed_hz);

    if (hid_write(device_.get(), report, report.size()) == -1)
    {
        SPDLOG_ERROR("Failed to send status/set command");
        return std::nullopt;
    }

    std::vector<uint8_t> response(64, 0);
    int bytes_read = hid_read(device_.get(), response.data(), response.size());
    if (bytes_read < 64)
    {
        SPDLOG_ERROR("Failed to read enough data for status response (read {} bytes)", bytes_read);
        return std::nullopt;
    }

    if ((response[0] != 0x10) || (response[1] != 0x00))
    {
        SPDLOG_ERROR("Status/set command failed with code 0x{:02x}", response[1]);
        return std::nullopt;
    }
    
    MCP2221AStatus status;

    // I2C Status
    status.i2c_cancel_response = static_cast<I2CCancelResponse>(response[2]);
    status.i2c_speed_response = static_cast<I2CSpeedResponse>(response[3]);
    status.speed_hz = 12000000 / (response[4] + 3);
    status.i2c_state = static_cast<I2CState>(response[8]);
    // Byte 20 bit 6 only; the surrounding bits are documented "don't care" and
    // are set in practice. See MCP2221AStatus::address_acked.
    status.address_acked = (response[20] & 0x40u) == 0u;

    return status;
}

bool MCP2221A::set_i2c_speed(uint32_t speed_hz)
{
    if (!is_open())
    {
        return false;
    }
    
    auto status = get_status_set_parameters(false, speed_hz);
    if (!status)
    {
        SPDLOG_ERROR("Failed to set I2C speed.");
        return false;
    }

    if (status->i2c_speed_response != I2CSpeedResponse::NowConsidered)
    {
        SPDLOG_ERROR("I2C speed not set, i2c_speed_response: 0x{:02x}, i2c_cancel_response: 0x{:02x}", static_cast<uint8_t>(status->i2c_speed_response), static_cast<uint8_t>(status->i2c_cancel_response));
        return false;
    }

    SPDLOG_INFO("I2C speed set to {} Hz", status->speed_hz);
    return true;
}

bool MCP2221A::cancel()
{
    if (!is_open())
    {
        return false;
    }

    // Cancelling with no transfer in progress is not a harmless no-op on this
    // part. The engine drives a STOP condition to unwind the transfer; with
    // nothing to unwind that STOP never completes and the state machine latches
    // StopTimeout (0x62), which refuses every subsequent parameter change and
    // survives process exit. Recovering from it needs a full device reset.
    // Verified on hardware: reset to Idle, one cancel, and the engine is stuck.
    //
    // So treat "already idle" as success and do not touch the bus.
    const auto current = get_status();
    if (!current)
    {
        return false;
    }
    if (current->i2c_state == I2CState::Idle)
    {
        return true;
    }

    auto status = get_status_set_parameters(true);
    if (!status) {
        SPDLOG_ERROR("Failed to cancel I2C.");
        return false;
    }

    if ((status->i2c_cancel_response != I2CCancelResponse::MarkedForCancellation) &&
        (status->i2c_cancel_response != I2CCancelResponse::AlreadyInIdleMode))
    {
        SPDLOG_WARN("I2C not canceled, i2c_cancel_response: 0x{:02x}", static_cast<uint8_t>(status->i2c_cancel_response));
        return false;
    }

    SPDLOG_INFO("I2C canceled");
    return true;
}

std::optional<MCP2221AStatus> MCP2221A::get_status()
{
    return get_status_set_parameters();
}

bool MCP2221A::i2c_write(uint8_t address, const std::vector<uint8_t>& data) {
    if (!is_open()) return false;
    if (data.size() > 60) {
        SPDLOG_ERROR("I2C write data too large (max 60 bytes)");
        return false;
    }

    auto report = makeI2cWrite(static_cast<uint16_t>(data.size()), static_cast<uint8_t>(address << 1));
    
    // Copy the data payload starting at index 4 (which is report[3] due to offset)
    for (size_t i = 0; i < data.size(); ++i) {
        report[4 + i] = data[i];
    }

    if (hid_write(device_.get(), report, report.size()) == -1) {
        SPDLOG_ERROR("Failed to send I2C write command");
        return false;
    }

    std::vector<uint8_t> response(64, 0);
    int res = hid_read_timeout(device_.get(), response.data(), response.size(), 100u);

    if ((res <= 0) || (response[0] != 0x90) || (response[1] != 0x00))
    {
        SPDLOG_ERROR("I2C write failed for address 0x{:02X}, res = {}, response[0]=0x{:02X}, response[1]=0x{:02X}", address, res, response[0], response[1]);
        return false;
    }

    // Accepting the command only means the engine started; it says nothing
    // about whether anything answered. Without this check a write to an empty
    // address "succeeds", and the failure surfaces later as an unexplained read
    // error -- which is exactly how the MFi coprocessor's silence used to
    // present.
    const auto status = get_status();
    if (status && !status->address_acked)
    {
        // DEBUG for the same reason as the read path below: a sleeping client
        // NACKs its first access and wakes on it, so the caller's retry is
        // expected to succeed. Raising this here made a normal wake-up look
        // like a fault in the log.
        SPDLOG_DEBUG("I2C write to 0x{:02X}: the client did not acknowledge its address "
                     "(engine state 0x{:02x})",
                     address, static_cast<uint8_t>(status->i2c_state));
        clear_i2c_engine();
        return false;
    }

    SPDLOG_DEBUG("Write to device 0x{:02X} = [{:02X}]", address, fmt::join(data, ", "));

    return true;
}

std::vector<uint8_t> MCP2221A::i2c_read(uint8_t address, size_t length)
{
    if (!is_open() || length == 0)
    {
        return {};
    }

    // Check if I2C engine is ready before starting
    auto status = get_status();
    if (status && status->i2c_state != I2CState::Idle)
    {
        SPDLOG_WARN("I2C engine not idle (state: 0x{:02x}), cancelling previous operation", static_cast<uint8_t>(status->i2c_state));
        cancel(); // Cancel any pending operation
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    // Send I2C read command with the total length
    auto report = makeI2cRead(static_cast<uint16_t>(length), static_cast<uint8_t>((address << 1) | 0x01));

    if (hid_write(device_.get(), report, report.size()) == -1)
    {
        SPDLOG_ERROR("Failed to send I2C read command");
        return {};
    }

    std::vector<uint8_t> response(64, 0);
    int res = hid_read_timeout(device_.get(), response.data(), response.size(), 100u);

    // Check if we got a valid response
    if ((res == 0) || (response[0] != 0x91) || (response[1] != 0x00))
    {
        SPDLOG_ERROR("I2C read failed: res={}, response[0]=0x{:02x}, response[1]=0x{:02x}", res, response[0], response[1]);
        return {};
    }
    
    // Now get the actual data using multiple 0x40 commands in 60-byte chunks
    std::vector<uint8_t> final_result;
    final_result.reserve(length);
    
    size_t total_bytes_read = 0;
    
    while (total_bytes_read < length)
    {
        // Try multiple times to get the data, as the I2C operation might still be in progress
        bool success = false;
        for (uint8_t attempt = 0; attempt < 3u; ++attempt)
        {
            // For longer reads this is necessary. TODO We could probably calculate if a delay
            // is required or not based on the length of the read.
            std::this_thread::sleep_for(std::chrono::milliseconds(10u));

            report = makeGetI2cData();

            if (hid_write(device_.get(), report, report.size()) == -1)
            {
                SPDLOG_ERROR("Failed to send I2C get data command at offset {}", total_bytes_read);
                return {};
            }
            
            res = hid_read_timeout(device_.get(), response.data(), response.size(), 100u);

            if (res <= 0)
            {
                SPDLOG_WARN("No response on attempt {} at offset {}", attempt, total_bytes_read);
                continue;
            }
            
            if (response[0] != 0x40)
            {
                SPDLOG_WARN("Wrong response type 0x{:02x} on attempt {} at offset {}", response[0], attempt, total_bytes_read);
                continue;
            }
            
            if (response[1] == 0x00)
            {
                // Success
                success = true;
                break;
            }
            else if (response[1] == 0x41)
            {
                // Not "busy" -- DS20005565E table 3-31 calls 0x41 "error reading
                // the I2C client data from the I2C engine". Retrying is worth a
                // couple of goes because the engine may still be clocking the
                // transfer out, but if it persists the transfer has failed and
                // the usual cause is that the address was never acknowledged.
                SPDLOG_DEBUG("I2C get-data error (0x41) on attempt {} at offset {}, retrying...",
                             attempt, total_bytes_read);
                continue;
            }
            else
            {
                // Other error
                SPDLOG_ERROR("I2C error 0x{:02x} on attempt {} at offset {}", response[1], attempt, total_bytes_read);
                break;
            }
        }
        
        if (!success)
        {
            // DEBUG, not ERROR: the failure is already reported through the
            // return value, and one failed transfer is not by itself a fault.
            // Clients that sleep -- the Apple MFi coprocessor is one -- NACK the
            // first access after an idle period and wake on it, so a single
            // failure here is the *normal* path and the caller's retry succeeds
            // milliseconds later. Only the caller knows when it has run out of
            // attempts, so only the caller should raise it; read_register() in
            // apple_mfi_ic.cpp does exactly that.
            const auto state = get_status();
            SPDLOG_DEBUG("I2C read from 0x{:02x} failed at offset {}: response[1]=0x{:02x}{}",
                         address, total_bytes_read, response[1],
                         (state && !state->address_acked)
                             ? " -- the client did not acknowledge its address"
                             : "");
            clear_i2c_engine();
            return {};
        }

        size_t bytes_available = response[3];
        // 127 is the datasheet's error marker for this field (table 3-31); the
        // bytes after it are meaningless and must not be copied.
        if (bytes_available == 127)
        {
            SPDLOG_ERROR("I2C read from 0x{:02x}: engine reported an error at offset {}", address,
                         total_bytes_read);
            clear_i2c_engine();
            return {};
        }
        if (bytes_available == 0)
        {
            // No more data available
            break;
        }
        
        // Copy the available data (up to 60 bytes per 0x40 response)
        size_t bytes_to_copy = (bytes_available > (length - total_bytes_read)) ? (length - total_bytes_read) : bytes_available;
        final_result.insert(final_result.end(), response.data() + 4, response.data() + 4 + bytes_to_copy);
        total_bytes_read += bytes_to_copy;
    }
    
    if (final_result.size() != length)
    {
        SPDLOG_WARN("Expected {} bytes but got {} bytes total", length, final_result.size());
    }
    
    SPDLOG_DEBUG("Read device 0x{:02X} = [{:02X}]", address, fmt::join(final_result, ", "));

    return final_result;
}

bool MCP2221A::clear_i2c_engine()
{
    // An address that goes unanswered leaves the engine latched in
    // AddressNACKed (0x25), and while it is latched *every* subsequent transfer
    // command is refused with 0x01. A scan therefore has to unwind the previous
    // address before probing the next one, or it only ever sees every other
    // address -- and the first unanswered one poisons the rest.
    //
    // Cancelling can take "a few hundreds of microseconds" (DS20005565E table
    // 3-2, byte 2), so this polls rather than assuming one round trip is enough.
    for (int attempt = 0; attempt < 5; ++attempt)
    {
        const auto current = get_status();
        if (!current)
        {
            return false;
        }
        if (current->i2c_state == I2CState::Idle)
        {
            return true;
        }
        get_status_set_parameters(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

std::vector<uint8_t> MCP2221A::scan_i2c_bus()
{
    std::vector<uint8_t> found_devices;
    if (!is_open()) return found_devices;

    // 0x00 is the general-call address and 0x78-0x7f are reserved; probing them
    // tells us nothing and 0x00 is answered by parts that are not really there.
    for (uint8_t addr = 0x03; addr < 0x78; ++addr)
    {
        if (!clear_i2c_engine())
        {
            SPDLOG_WARN("I2C engine will not return to idle; abandoning the scan at 0x{:02x}",
                        addr);
            break;
        }

        // A zero-length write is a full START-ADDR-STOP with no payload, which
        // is the least intrusive way to ask "is anybody there".
        auto report = makeI2cWrite(0, static_cast<uint8_t>(addr << 1));

        if (hid_write(device_.get(), report, report.size()) == -1)
        {
            SPDLOG_WARN("hid_write failed during scan for 0x{:02x}", addr);
            continue;
        }

        std::vector<uint8_t> response(64, 0);
        const int res = hid_read_timeout(device_.get(), response.data(), response.size(), 50);
        if ((res <= 0) || (response[0] != 0x90) || (response[1] != 0x00))
        {
            continue;
        }

        // The command being accepted only means the engine took it. The address
        // has not been clocked out yet, so the ACK bit and the state machine
        // are both still stale -- read them immediately and every address looks
        // unanswered. At 100 kHz an address byte is ~90 us; 2 ms is generous
        // and still scans the whole range in well under a second.
        std::this_thread::sleep_for(std::chrono::milliseconds(2));

        // Two independent tells, and they should agree: the ACK bit, and the
        // state machine parking at AddressNACKed. Requiring both means a
        // misread of either one cannot invent a device.
        const auto status = get_status();
        if (status && status->address_acked && status->i2c_state != I2CState::AddressNACKed)
        {
            found_devices.push_back(addr);
        }
    }

    clear_i2c_engine();
    return found_devices;
}
