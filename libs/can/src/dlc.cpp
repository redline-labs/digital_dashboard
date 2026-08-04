// SPDX-License-Identifier: GPL-3.0-or-later

#include "can/dlc.h"

namespace can
{
namespace
{

constexpr uint8_t kDlcToLength[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64 };

} // namespace

uint8_t dlc_to_length(uint8_t dlc, bool fd)
{
    const uint8_t index = dlc & 0x0F;
    if (!fd)
    {
        // Classic CAN allows DLC 9..15 on the wire but they all mean eight
        // bytes; a controller that reported one is not lying, it is being
        // literal about a field the standard leaves loose.
        return index > 8 ? 8 : index;
    }
    return kDlcToLength[index];
}

uint8_t length_to_dlc(uint8_t length, bool fd)
{
    if (!fd)
    {
        return length > 8 ? 8 : length;
    }
    for (uint8_t dlc = 0; dlc < 16; ++dlc)
    {
        if (kDlcToLength[dlc] >= length)
        {
            return dlc;
        }
    }
    return 15;
}

bool is_valid_can_length(uint8_t length, bool fd)
{
    if (!fd)
    {
        return length <= 8;
    }
    for (uint8_t dlc = 0; dlc < 16; ++dlc)
    {
        if (kDlcToLength[dlc] == length)
        {
            return true;
        }
    }
    return false;
}

uint8_t round_up_can_length(uint8_t length, bool fd)
{
    return dlc_to_length(length_to_dlc(length, fd), fd);
}

} // namespace can
