// SPDX-License-Identifier: GPL-3.0-or-later
#include "i2c_bus/i2c_bus.h"

#include <spdlog/spdlog.h>

#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace i2c
{
namespace
{

// The bench adapter is the MCP2221A exposed by the in-kernel hid_mcp2221
// driver, which names itself in /sys/class/i2c-dev/i2c-N/name.
constexpr const char* kPreferredAdapterSubstring = "MCP2221";

std::string readAdapterName(const fs::path& sysfs_dir)
{
    std::ifstream in(sysfs_dir / "name");
    std::string name;
    std::getline(in, name);
    return name;
}

// Prefers the MCP2221A adapter when several buses exist, since a deployed board
// will have unrelated ones (HDMI DDC, PMIC, sensors) that we must not poke.
std::string autoDetectDevice()
{
    std::string fallback;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator("/sys/class/i2c-dev", ec))
    {
        const std::string bus = entry.path().filename().string();
        const std::string device = "/dev/" + bus;
        const std::string name = readAdapterName(entry.path());

        if (name.find(kPreferredAdapterSubstring) != std::string::npos)
        {
            SPDLOG_DEBUG("[i2c] auto-detected {} ({})", device, name);
            return device;
        }
        if (fallback.empty())
        {
            fallback = device;
        }
    }
    if (ec)
    {
        SPDLOG_DEBUG("[i2c] cannot enumerate /sys/class/i2c-dev: {}", ec.message());
    }
    return fallback;
}

class LinuxI2cDevBus : public Bus
{
  public:
    explicit LinuxI2cDevBus(std::string device) : device_(std::move(device)) {}

    ~LinuxI2cDevBus() override { close(); }

    bool open() override
    {
        if (device_.empty())
        {
            device_ = autoDetectDevice();
        }
        if (device_.empty())
        {
            SPDLOG_ERROR("[i2c] no I2C adapter found. Is i2c-dev loaded, and (on a bench "
                         "setup) hid_mcp2221 bound to the bridge? See "
                         "nodes/carplay/udev/carplay-i2c.conf.");
            return false;
        }

        fd_ = ::open(device_.c_str(), O_RDWR);
        if (fd_ < 0)
        {
            SPDLOG_ERROR("[i2c] cannot open {}: {}. Install the udev rules from "
                         "nodes/carplay/udev/99-carplay.rules and check you are in plugdev.",
                         device_, std::strerror(errno));
            return false;
        }

        unsigned long functionality = 0;
        if (::ioctl(fd_, I2C_FUNCS, &functionality) >= 0 &&
            (functionality & I2C_FUNC_I2C) == 0)
        {
            SPDLOG_ERROR("[i2c] {} does not support plain I2C transfers (I2C_RDWR)", device_);
            close();
            return false;
        }

        SPDLOG_INFO("[i2c] using {}", description());
        return true;
    }

    void close() override
    {
        if (fd_ >= 0)
        {
            ::close(fd_);
            fd_ = -1;
        }
    }

    bool is_open() const override { return fd_ >= 0; }

    bool write(uint8_t address, const std::vector<uint8_t>& data) override
    {
        if (!is_open())
        {
            return false;
        }

        // A zero-length write still needs a valid buffer pointer for the ioctl.
        uint8_t placeholder = 0;
        i2c_msg message{};
        message.addr = address;
        message.flags = 0;
        message.len = static_cast<uint16_t>(data.size());
        message.buf = data.empty() ? &placeholder : const_cast<uint8_t*>(data.data());

        return transfer(&message, 1);
    }

    std::vector<uint8_t> read(uint8_t address, size_t length) override
    {
        if (!is_open() || length == 0)
        {
            return {};
        }

        std::vector<uint8_t> buffer(length);
        i2c_msg message{};
        message.addr = address;
        message.flags = I2C_M_RD;
        message.len = static_cast<uint16_t>(length);
        message.buf = buffer.data();

        if (!transfer(&message, 1))
        {
            return {};
        }
        return buffer;
    }

    bool probe(uint8_t address) override
    {
        // Mirrors i2cdetect's read-byte probe. A zero-length write is rejected
        // by some adapters, so read a byte and discard it.
        return !read(address, 1).empty();
    }

    std::string description() const override
    {
        std::string name;
        if (!device_.empty())
        {
            const std::string bus = fs::path(device_).filename().string();
            name = readAdapterName(fs::path("/sys/class/i2c-dev") / bus);
        }
        return name.empty() ? device_ : device_ + " (" + name + ")";
    }

  private:
    bool transfer(i2c_msg* messages, unsigned count)
    {
        i2c_rdwr_ioctl_data payload{};
        payload.msgs = messages;
        payload.nmsgs = count;

        if (::ioctl(fd_, I2C_RDWR, &payload) < 0)
        {
            SPDLOG_DEBUG("[i2c] transfer to 0x{:02x} failed: {}", messages[0].addr,
                         std::strerror(errno));
            return false;
        }
        return true;
    }

    std::string device_;
    int fd_ = -1;
};

}  // namespace

std::unique_ptr<Bus> makeLinuxI2cDevBus(const std::string& device)
{
    return std::make_unique<LinuxI2cDevBus>(device);
}

}  // namespace i2c
