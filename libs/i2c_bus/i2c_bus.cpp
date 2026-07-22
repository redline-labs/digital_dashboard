// SPDX-License-Identifier: GPL-3.0-or-later
#include "i2c_bus/i2c_bus.h"

#include <spdlog/spdlog.h>

namespace i2c
{

#ifdef I2C_BUS_HAVE_LINUX_I2CDEV
std::unique_ptr<Bus> makeLinuxI2cDevBus(const std::string& device);
#endif
#ifdef I2C_BUS_HAVE_MCP2221A
std::unique_ptr<Bus> makeMcp2221aBus();
#endif

std::vector<uint8_t> Bus::scan()
{
    std::vector<uint8_t> found;
    // 0x00-0x02 and 0x78-0x7f are reserved; skipping them matches i2cdetect.
    for (uint8_t address = 0x03; address < 0x78; ++address)
    {
        if (probe(address))
        {
            found.push_back(address);
        }
    }
    return found;
}

std::unique_ptr<Bus> makeBus(const std::string& hint)
{
#ifdef I2C_BUS_HAVE_LINUX_I2CDEV
    return makeLinuxI2cDevBus(hint);
#elif defined(I2C_BUS_HAVE_MCP2221A)
    (void)hint;
    return makeMcp2221aBus();
#else
    (void)hint;
    SPDLOG_ERROR("[i2c] no I2C backend was compiled in");
    return nullptr;
#endif
}

}  // namespace i2c
