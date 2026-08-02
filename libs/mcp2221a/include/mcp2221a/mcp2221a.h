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

    // Whether the client acknowledged its address on the last transfer.
    //
    // This is byte 20 bit 6 of the Status/Set Parameters response, and only
    // that bit: the datasheet (DS20005565E table 3-2) marks bit 7 and bits 5-0
    // "don't care", and on real hardware they are not zero. Comparing the whole
    // byte to zero -- which this used to do -- therefore reads as "NACK" always,
    // which is why scan_i2c_bus() never found anything on a working bus.
    //
    // Sense is inverted on the wire: the datasheet says "if ACK was received
    // from client value is 0, else 1", so this field is the negation.
    bool address_acked;
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

    // Unwind a latched transfer so the engine is idle. A NACKed address leaves
    // it stuck, and while stuck every transfer command is refused.
    bool clear_i2c_engine();

    // Resets the device and waits for it to re-enumerate into a usable state.
    // Only called when the I2C engine is found latched in a non-idle state,
    // because it costs a full USB re-enumeration.
    bool reset_and_reopen();

    std::unique_ptr<hid_device, void(*)(hid_device*)> device_;

    static constexpr uint16_t kVendorId = 0x04D8;
    static constexpr uint16_t kProductId = 0x00DD;
};
