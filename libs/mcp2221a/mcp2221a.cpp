#include "mcp2221a/mcp2221a.h"

#include <spdlog/spdlog.h>
#include <spdlog/fmt/ranges.h> // Required for fmt::join

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

// Generic make_report function that accepts variable arguments
template<typename... Args>
constexpr Report_t make_report(MCP2221ACommands cmd, Args... args)
{
    Report_t report(cmd);
    size_t index = 1;
    
    // Fold expression to set each argument
    ((report[index++] = static_cast<uint8_t>(args)), ...);
    
    return report;
}

// Template function with enum literal as non-type template parameter
template<MCP2221ACommands Cmd, typename... Args>
constexpr Report_t make_report(Args... args)
{
    Report_t report(Cmd);
    size_t index = 1u;
    
    // Fold expression to set each argument
    ((report[index++] = static_cast<uint8_t>(args)), ...);
    
    return report;
}

// Template specialization for Reset command with default values
template<>
constexpr Report_t make_report<MCP2221ACommands::Reset>()
{
    return make_report(MCP2221ACommands::Reset, 0xAB, 0xCD, 0xEF);
}

// Template specialization for I2C Write Data command
template<>
constexpr Report_t make_report<MCP2221ACommands::I2CWriteData, uint16_t, uint8_t>(uint16_t data_length, uint8_t address)
{
    return make_report(MCP2221ACommands::I2CWriteData, 
                       static_cast<uint8_t>(data_length & 0xFF),        // Length LSB
                       static_cast<uint8_t>((data_length >> 8) & 0xFF), // Length MSB
                       address);                                         // I2C address
}

// Template specialization for I2C Read Data command
template<>
constexpr Report_t make_report<MCP2221ACommands::I2CReadData, uint16_t, uint8_t>(uint16_t data_length, uint8_t address)
{
    return make_report(MCP2221ACommands::I2CReadData,
                       static_cast<uint8_t>(data_length & 0xFF),        // Length LSB
                       static_cast<uint8_t>((data_length >> 8) & 0xFF), // Length MSB
                       address);                                         // I2C address with read bit
}

// Template specialization for I2C Get Data command
template<>
constexpr Report_t make_report<MCP2221ACommands::I2CGetData>()
{
    return make_report(MCP2221ACommands::I2CGetData);
}

template<>
constexpr Report_t make_report<MCP2221ACommands::StatusSetParameters, bool, uint32_t>(bool cancel_i2c, uint32_t speed_hz)
{
    return make_report(
        MCP2221ACommands::StatusSetParameters,
        0x00, // Reserved
        cancel_i2c ? 0x10 : 0x00,
        speed_hz > 1000u ? 0x20 : 0x00,
        (12000000 / speed_hz) - 3
    );
}


MCP2221A::MCP2221A() :
    device_{nullptr, hid_close}
{
    if (hid_init())
    {
        spdlog::error("Failed to initialize HIDAPI");
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
        SPDLOG_ERROR("MCP2221A device not found.");
        return false;
    }
    
    // Reset the device.
    auto report = make_report<MCP2221ACommands::Reset>();

    if (hid_write(device_.get(), report, report.size()) == -1)
    {
        SPDLOG_ERROR("Failed to send reset command");
        return false;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    device_ = {hid_open(kVendorId, kProductId, nullptr), hid_close};
    if (!device_)
    {
        SPDLOG_ERROR("MCP2221A device not found after reset.");
        return false;
    }

    return true;
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

    auto report = make_report<MCP2221ACommands::StatusSetParameters>(cancel_i2c, speed_hz);

    if (hid_write(device_.get(), report, report.size()) == -1)
    {
        SPDLOG_ERROR("Failed to send status/set command");
        return std::nullopt;
    }

    std::vector<uint8_t> response(64, 0);
    int bytes_read = hid_read(device_.get(), response.data(), response.size());
    if (bytes_read < 64u)
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
    status.ack_status = response[20];

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

    auto report = make_report<MCP2221ACommands::I2CWriteData>(data.size(), address << 1);
    
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

    if (response[0] != 0x90 || response[1] != 0x00) {
        SPDLOG_ERROR("I2C write failed for address 0x{:02X}, response[0]=0x{:02X}, response[1]=0x{:02X}", address, response[0], response[1]);
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
    auto report = make_report<MCP2221ACommands::I2CReadData>(length, (address << 1) | 0x01);

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

            report = make_report<MCP2221ACommands::I2CGetData>();

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
                // I2C engine busy, wait and retry
                SPDLOG_DEBUG("I2C engine busy (0x41) on attempt {} at offset {}, retrying...", attempt, total_bytes_read);
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
            SPDLOG_ERROR("Failed to get I2C read data at offset {} after 10 attempts: res={}, response[0]=0x{:02x}, response[1]=0x{:02x}", total_bytes_read, res, response[0], response[1]);
            cancel();
            return {};
        }
        
        size_t bytes_available = response[3];
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

std::vector<uint8_t> MCP2221A::scan_i2c_bus()
{
    std::vector<uint8_t> found_devices;
    if (!is_open()) return found_devices;

    for (uint8_t addr = 1; addr < 128; ++addr) {
        // Use I2C Write Data command with 0 length to ping the address.
        // This performs a full START-ADDR-STOP sequence.
        auto report = make_report<MCP2221ACommands::I2CWriteData>(0, addr << 1);

        if (hid_write(device_.get(), report, report.size()) == -1)
        {
            SPDLOG_WARN("hid_write failed during scan for 0x{:02x}", addr);
            continue;
        }

        std::vector<uint8_t> response(64, 0);
        int res = hid_read_timeout(device_.get(), response.data(), response.size(), 10);

        if ((res > 0) && (response[0] == 0x90) && (response[1] == 0x00))
        {
            // Now check the ACK status.
            auto status = get_status();
            if (status && status->ack_status == 0x00)
            {
                found_devices.push_back(addr);
                //SPDLOG_INFO("Found device at address 0x{:02x}", addr);
             }
        }
        else
        {
            // This can happen if the bus is locked. Issue a cancel.
            get_status_set_parameters(true);
        }

    }

    return found_devices;
}
