// SPDX-License-Identifier: GPL-3.0-or-later
#include "i2c_bus/i2c_bus.h"

#include "mcp2221a/mcp2221a.h"

#include <spdlog/spdlog.h>

namespace i2c
{
namespace
{

constexpr uint32_t kBusSpeedHz = 100000;

// The MCP2221A driven straight over USB HID. Used on macOS, which has no kernel
// driver for the bridge; on Linux the kernel's hid_mcp2221 exposes the same
// chip as a normal I2C adapter and that path is preferred.
class Mcp2221aBus : public Bus
{
  public:
    ~Mcp2221aBus() override { close(); }

    bool open() override
    {
        if (!device_.open())
        {
            return false;
        }
        if (!device_.set_i2c_speed(kBusSpeedHz))
        {
            SPDLOG_ERROR("[i2c] cannot set the MCP2221A bus speed");
            return false;
        }
        SPDLOG_INFO("[i2c] using {}", description());
        return true;
    }

    void close() override {}

    bool is_open() const override { return device_.is_open(); }

    bool write(uint8_t address, const std::vector<uint8_t>& data) override
    {
        return device_.i2c_write(address, data);
    }

    std::vector<uint8_t> read(uint8_t address, size_t length) override
    {
        return device_.i2c_read(address, length);
    }

    bool probe(uint8_t address) override { return !read(address, 1).empty(); }

    std::string description() const override { return "MCP2221A over USB HID (hidapi)"; }

  private:
    mutable MCP2221A device_;
};

}  // namespace

std::unique_ptr<Bus> makeMcp2221aBus()
{
    return std::make_unique<Mcp2221aBus>();
}

}  // namespace i2c
