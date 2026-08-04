// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the reconfiguration tool is being asked to do, read from a YAML file.
//
// Every target field is an optional, and an absent one means LEAVE IT ALONE.
// That is the opposite of the convention the CarPlay node's loader uses, where
// an absent key takes a documented default, and the difference is deliberate:
// a runtime config that falls back to a default produces a working session,
// whereas a reconfiguration that falls back to a default silently rewrites a
// COB-ID the operator never mentioned, into non-volatile memory, on a device
// that can only be recovered by guessing its bit rate. Absent has to mean
// absent here.
#ifndef GRAYHILL_RECONFIGURE_CONFIG_H
#define GRAYHILL_RECONFIGURE_CONFIG_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace grayhill
{

// How to reach the keypad right now. Both are required: the wrong bit rate is
// indistinguishable from an absent keypad, so guessing one is not a kindness.
struct CurrentAddress
{
    uint8_t nodeId { 0 };
    uint32_t bitrateKbps { 0 };
};

struct TargetCobIds
{
    std::optional<uint32_t> buttons;    // TPDO1 -> 0x1800:01
    std::optional<uint32_t> indicators; // RPDO1 -> 0x1400:01
    std::optional<uint32_t> brightness; // RPDO2 -> 0x1401:01

    bool any() const
    {
        return buttons.has_value() || indicators.has_value() || brightness.has_value();
    }
};

struct TargetState
{
    std::optional<uint8_t> nodeId;
    std::optional<uint32_t> bitrateKbps;
    TargetCobIds cobIds;

    std::optional<uint16_t> heartbeatMs;    // 0x1017:00
    std::optional<uint16_t> eventTimerMs;   // 0x1800:05
    std::optional<uint16_t> inhibitTimeUs;  // 0x1800:03, in units of 100 us
    std::optional<uint8_t> transmissionType; // 0x1800:02
    std::optional<uint16_t> indicatorScalar; // 0x2010:01
    std::optional<uint16_t> backlightScalar; // 0x2010:02

    // Shorthand for the pair of values MoTeC's PDM Manager checks before it
    // will configure a keypad: transmission type and backlight scalar both
    // 0xFE. Setting this alongside an explicit conflicting value for either is
    // an error rather than a silent override -- the operator has said two
    // different things and only they can say which they meant.
    bool motecCompatible { false };
};

struct ReconfigConfig
{
    CurrentAddress current;
    TargetState target;

    // 0x1010:01 <- "save". Without it nothing survives the next power cycle,
    // so it defaults on; turning it off is for trying something out.
    bool store { true };
    // NMT reset node, then wait for the boot-up frame.
    bool resetAfter { true };
};

// Reads a config file. On failure the vector holds one message per problem,
// phrased for an operator rather than a programmer, and nothing is returned.
std::optional<ReconfigConfig> load_reconfig_config(const std::string& path,
                                                   std::vector<std::string>& errors);

// The same, from text already in hand. Exposed for tests.
std::optional<ReconfigConfig> parse_reconfig_config(const std::string& yaml,
                                                    std::vector<std::string>& errors);

} // namespace grayhill

#endif // GRAYHILL_RECONFIGURE_CONFIG_H
