#include "mcp2221a/mcp2221a.h"
#include <spdlog/spdlog.h>
#include <vector>
#include <cstring>
#include <iostream>
#include <optional>
#include <thread>

MCP2221A::MCP2221A() : device_(nullptr) {
    if (hid_init()) {
        spdlog::error("Failed to initialize HIDAPI");
    }
}

MCP2221A::~MCP2221A() {
    close();
    hid_exit();
}

bool MCP2221A::open() {
    device_ = hid_open(VENDOR_ID, PRODUCT_ID, nullptr);
    if (!device_) {
        spdlog::error("MCP2221A device not found");
        return false;
    }
    spdlog::info("MCP2221A device opened");

    // Cancel any pending I2C transfers
    get_status_set_parameters(true);

    return true;
}

void MCP2221A::close() {
    if (device_) {
        hid_close(device_);
        device_ = nullptr;
    }
}

bool MCP2221A::is_open() const {
    return device_ != nullptr;
}

std::optional<MCP2221AStatus> MCP2221A::get_status_set_parameters(bool cancel_i2c, uint32_t speed_hz)
{
    if (!is_open()) return std::nullopt;

    std::vector<uint8_t> report(65, 0);
    report[0] = 0x00; // Report ID
    report[1] = 0x10; // Command: Status/Set Parameters
    report[2] = 0x00; // Reserved

    if (cancel_i2c == true)
    {
        report[3] = 0x10; // Cancel current I2C/SMBus transfer
    }
    else
    {
        report[3] = 0x00; // No action
    }

    if(speed_hz > 1000)
    {
        report[4] = 0x20; // Set I2C parameters
        report[5] = (12000000 / speed_hz) - 3;
    }
    else
    {
        report[4] = 0x00; // No action
        report[5] = 0x00; // No action
    }

    if (hid_write(device_, report.data(), report.size()) == -1) {
        spdlog::error("Failed to send status/set command");
        return std::nullopt;
    }

    std::vector<uint8_t> response(64, 0);
    int bytes_read = hid_read(device_, response.data(), response.size());
    if (bytes_read < 64) {
        spdlog::error("Failed to read enough data for status response (read {} bytes)", bytes_read);
        return std::nullopt;
    }

    if (response[0] != 0x10 || response[1] != 0x00) {
        spdlog::error("Status/set command failed with code 0x{:02x}", response[1]);
        return std::nullopt;
    }
    
    MCP2221AStatus status;

    // I2C Status
    status.i2c_cancel_response = static_cast<I2CCancelResponse>(response[2]);
    status.i2c_speed_response = static_cast<I2CSpeedResponse>(response[3]);
    status.speed_hz = 12000000 / (response[4] + 3); // sram[6] is the divider from status read
    status.i2c_state = static_cast<I2CState>(response[8]);
    status.ack_status = response[20];

    //SPDLOG_INFO("Cancel I2C: {}, Speed response: {}, Speed: {} Hz, State I2C: {}, Ack status: {}", static_cast<uint8_t>(status.i2c_cancel_response), static_cast<uint8_t>(status.i2c_speed_response), status.speed_hz, static_cast<uint8_t>(status.i2c_state), status.ack_status);

    return status;
}

bool MCP2221A::set_i2c_speed(uint32_t speed_hz) {
    if (!is_open())
    {
        return false;
    }
    
    auto status = get_status_set_parameters(false, speed_hz);
    if (!status) {
        spdlog::error("Failed to set I2C speed.");
        return false;
    }

    if (status->i2c_speed_response != I2CSpeedResponse::NowConsidered) {
        spdlog::error("I2C speed not set, i2c_speed_response: 0x{:02x}", static_cast<uint8_t>(status->i2c_speed_response));
        return false;
    }

    spdlog::info("I2C speed set to {} Hz", status->speed_hz);
    return true;
}

std::optional<MCP2221AStatus> MCP2221A::get_status()
{
    return get_status_set_parameters();
}

bool MCP2221A::i2c_write(uint8_t address, const std::vector<uint8_t>& data) {
    if (!is_open()) return false;
    if (data.size() > 60) {
        spdlog::error("I2C write data too large (max 60 bytes)");
        return false;
    }

    std::vector<uint8_t> report(65, 0);
    report[0] = 0x00;
    report[1] = 0x90; // I2C Write Data
    report[2] = data.size() & 0xFF;
    report[3] = (data.size() >> 8) & 0xFF;
    report[4] = address << 1;

    memcpy(report.data() + 5, data.data(), data.size());

    if (hid_write(device_, report.data(), report.size()) == -1) {
        spdlog::error("Failed to send I2C write command");
        return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    std::vector<uint8_t> response(64, 0);
    int res = hid_read_timeout(device_, response.data(), response.size(), 100);

    if (response[0] != 0x90 || response[1] != 0x00) {
         spdlog::error("I2C write failed for address 0x{:02x}", address);
        return false;
    }

    return true;
}

std::vector<uint8_t> MCP2221A::i2c_read(uint8_t address, size_t length) {
    if (!is_open() || length > 60) return {};
    
    // Check if I2C engine is ready before starting
    auto status = get_status();
    if (status && status->i2c_state != I2CState::Idle) {
        spdlog::warn("I2C engine not idle (state: 0x{:02x}), cancelling previous operation", static_cast<uint8_t>(status->i2c_state));
        get_status_set_parameters(true); // Cancel any pending operation
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    std::vector<uint8_t> report(65, 0);
    report[0] = 0x00;
    report[1] = 0x91; // I2C Read Data
    report[2] = length & 0xFF;
    report[3] = (length >> 8) & 0xFF;
    report[4] = (address << 1) | 0x01;

    if (hid_write(device_, report.data(), report.size()) == -1) {
        SPDLOG_ERROR("Failed to send I2C read command");
        return {};
    }

    std::vector<uint8_t> response(64, 0);
    
    // Wait for the I2C read command to complete
    for (int i = 0; i < 10; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

        int res = hid_read_timeout(device_, response.data(), response.size(), 100);

        spdlog::debug("attempt = {}, res = {}, I2C read command response: 0x{:02x}, 0x{:02x}, 0x{:02x}", i, res, response[0], response[1], response[2]);

        // Check if we got a valid response
        if (res > 0 && response[0] == 0x91 && response[1] == 0x00) {
            break;
        }

        if (i == 9) {
            spdlog::error("I2C read command failed after {} attempts", i + 1);
            get_status_set_parameters(true); // Cancel the operation
            return {};
        }
    }

    // Now get the actual data
    std::fill(report.begin(), report.end(), 0);
    report[0] = 0x00;
    report[1] = 0x40; // I2C Read Data - Get Data

    if (hid_write(device_, report.data(), report.size()) == -1) {
        spdlog::error("Failed to send I2C get data command");
        return {};
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    int res = hid_read_timeout(device_, response.data(), response.size(), 100);

    spdlog::debug("res = {}, I2C read data response: 0x{:02x}, 0x{:02x}, 0x{:02x}, 0x{:02x}", res, response[0], response[1], response[2], response[3]);

    if (res <= 0 || response[0] != 0x40 || response[1] != 0x00) {
        spdlog::error("Failed to get I2C read data: res={}, response[0]=0x{:02x}, response[1]=0x{:02x}", res, response[0], response[1]);
        get_status_set_parameters(true); // Cancel the operation
        return {};
    }
    
    size_t bytes_read = response[3];
    if (bytes_read != length) {
        spdlog::warn("Expected {} bytes but got {} bytes", length, bytes_read);
    }
    
    std::vector<uint8_t> result(bytes_read);
    memcpy(result.data(), response.data() + 4, bytes_read);
    spdlog::debug("I2C read successful: {} bytes", result.size());

    return result;
}

std::vector<uint8_t> MCP2221A::scan_i2c_bus() {
    std::vector<uint8_t> found_devices;
    if (!is_open()) return found_devices;

    spdlog::info("Scanning I2C bus...");
    for (uint8_t addr = 1; addr < 128; ++addr) {
        // Use I2C Write Data command with 0 length to ping the address.
        // This performs a full START-ADDR-STOP sequence.
        std::vector<uint8_t> report(65, 0);
        report[0] = 0x00;
        report[1] = 0x90; // I2C Write Data (with STOP)
        report[2] = 0x00; // Length LSB
        report[3] = 0x00; // Length MSB
        report[4] = addr << 1; 

        if (hid_write(device_, report.data(), report.size()) == -1) {
            spdlog::warn("hid_write failed during scan for 0x{:02x}", addr);
            continue;
        }

        std::vector<uint8_t> response(64, 0);
        int res = hid_read_timeout(device_, response.data(), response.size(), 10);
        //SPDLOG_WARN("addr: 0x{:02x}, res: {}, response[0]: 0x{:02x}, response[1]: 0x{:02x}, response[2]: 0x{:02x}", addr, res, response[0], response[1], response[2]);
        
        if (res > 0 && response[0] == 0x90 && response[1] == 0x00) {
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

        //std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    spdlog::info("I2C scan complete.");
    return found_devices;
} 