// SPDX-License-Identifier: GPL-3.0-or-later

#include "can/error_injection.h"

#include <spdlog/fmt/fmt.h>

namespace can
{

ErrorInjection ErrorInjection::next_frame(uint16_t bitPosition)
{
    ErrorInjection injection;
    injection.mode = Mode::NextFrame;
    injection.bitPosition = bitPosition;
    return injection;
}

ErrorInjection ErrorInjection::targeted(uint32_t identifier, uint16_t bitPosition,
                                        uint16_t destroyCount, uint16_t passCount,
                                        uint8_t repeatCount)
{
    ErrorInjection injection;
    injection.mode = Mode::Targeted;
    injection.identifier = identifier;
    injection.bitPosition = bitPosition;
    injection.destroyCount = destroyCount;
    injection.passCount = passCount;
    injection.repeatCount = repeatCount;
    return injection;
}

Result<void> validate(const ErrorInjection& injection)
{
    switch (injection.mode)
    {
    case ErrorInjection::Mode::Off:
        return {};

    case ErrorInjection::Mode::NextFrame:
    case ErrorInjection::Mode::Targeted:
        if (injection.bitPosition > kMaxErrorInjectionBitPosition)
        {
            return invalid_argument(fmt::format("bit position {} is past the last one, {}",
                                                injection.bitPosition,
                                                kMaxErrorInjectionBitPosition));
        }
        if (injection.mode == ErrorInjection::Mode::NextFrame)
        {
            return {};
        }
        if (injection.identifier > 0x1FFFFFFFu)
        {
            return invalid_argument(
                fmt::format("identifier 0x{:X} is wider than 29 bits", injection.identifier));
        }
        // A cycle that destroys nothing would be accepted by the adapter and
        // do nothing, which reads as "the bus shrugged it off".
        if (injection.destroyCount == 0)
        {
            return invalid_argument("a targeted injection must destroy at least one frame");
        }
        return {};
    }
    return invalid_argument("unknown error injection mode");
}

const char* to_string(ErrorInjection::Mode mode)
{
    switch (mode)
    {
    case ErrorInjection::Mode::Off: return "off";
    case ErrorInjection::Mode::NextFrame: return "next-frame";
    case ErrorInjection::Mode::Targeted: return "targeted";
    }
    return "unknown";
}

} // namespace can
