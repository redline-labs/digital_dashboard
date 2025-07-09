#pragma once

#include <hidapi.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

enum class I2CState : uint8_t
{
    Idle = 0x00,
    StartSent = 0x10,
    StartTimeout = 0x12,
    AddressSent = 0x20,
    AddressSent_WaitingSendData = 0x21,
    AddressTxTimeout = 0x23,
    AddressNACKed = 0x25,
    MasterWaitingSendData = 0x41,
    MasterDataWriteTimeout = 0x44,
    MasterReadDataTimeout = 0x52,
    MasterReadAllData = 0x55,
    StopTimeout = 0x62,
    Unknown = 0xFF
};

enum class I2CCancelResponse: uint8_t
{
    NoSpecialTransfer = 0x00,
    MarkedForCancellation = 0x10,
    AlreadyInIdleMode = 0x11
};

enum class I2CSpeedResponse: uint8_t
{
    NoNewSpeedIssued = 0x00,
    NowConsidered = 0x20,
    NotSet = 0x21
};

struct MCP2221AStatus
{
    // I2C Status
    I2CCancelResponse i2c_cancel_response;
    I2CSpeedResponse i2c_speed_response;
    uint32_t speed_hz;
    I2CState i2c_state;
    uint8_t ack_status;
};

enum class MCP2221ACommands : uint8_t
{
    StatusSetParameters = 0x10,
    I2CWriteData = 0x90,
    I2CReadData = 0x91,
    I2CGetData = 0x40,

    Reset = 0x70,
};


class MCP2221A {
public:
    MCP2221A();
    ~MCP2221A();

    bool open();
    bool is_open() const;

    std::optional<MCP2221AStatus> get_status();
    bool set_i2c_speed(uint32_t speed_hz);
    bool cancel();

    bool i2c_write(uint8_t address, const std::vector<uint8_t>& data);
    std::vector<uint8_t> i2c_read(uint8_t address, size_t length);

    std::vector<uint8_t> scan_i2c_bus();

private:
    std::optional<MCP2221AStatus> get_status_set_parameters(bool cancel_i2c = false, uint32_t speed_hz = 0);

    std::unique_ptr<hid_device, void(*)(hid_device*)> device_;

    static constexpr uint16_t kVendorId = 0x04D8;
    static constexpr uint16_t kProductId = 0x00DD;
};
