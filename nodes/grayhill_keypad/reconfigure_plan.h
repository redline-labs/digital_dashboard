// SPDX-License-Identifier: GPL-3.0-or-later
//
// A desired state turned into an ordered list of things to do on the wire.
//
// The ordering is the whole design, and it is not cosmetic:
//
//   1. SDO writes, at the *current* node ID.
//   2. Store parameters -- 0x1010:01 <- "save".
//   3. Reset, and wait for the boot-up frame.
//   4. LSS: enter configuration, node ID, bit rate, store, exit.
//   5. Reset again. Everything after this speaks at the NEW node ID and the
//      NEW bit rate.
//
// Change your own addressing last, or you lose the device mid-sequence and a
// partially-applied configuration becomes unrecoverable without a bit rate
// sweep. This is the same order MoTeC's own tool uses, for the same reason.
//
// Widths come from the EDS rather than from a table here. That is not
// fastidiousness: the device enforces them, and the difference between writing
// 0x2010:02 as one byte and as two is the difference between a keypad PDM
// Manager accepts and one it rejects.
#ifndef GRAYHILL_RECONFIGURE_PLAN_H
#define GRAYHILL_RECONFIGURE_PLAN_H

#include "reconfigure_config.h"

#include "canopen/eds_ast.h"
#include "canopen/lss.h"
#include "canopen/nmt.h"
#include "canopen/sdo.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace grayhill
{

struct Step
{
    enum class Kind
    {
        NmtCommand,
        SdoWrite,
        SdoStore,
        NmtResetAndWait,
        // Wait for a boot-up frame without sending the reset that causes it.
        // Needed after an LSS reset, where the boot-up arrives at the device's
        // NEW node ID and NEW bit rate: the tool has to move to those before
        // it can hear the frame, so the reset and the wait cannot be one step.
        NmtWaitBootup,
        // Read an object back and compare it with what was written.
        Verify,
        LssEnter,
        LssNodeId,
        LssBitrate,
        LssStore,
        LssExit,
        // Everything after this point addresses the device differently.
        Readdress,
    };

    Kind kind { Kind::NmtCommand };
    std::string description;

    // SdoWrite / SdoStore / Verify
    uint16_t index { 0 };
    uint8_t sub { 0 };
    uint64_t value { 0 };
    uint8_t width { 0 };

    // NmtCommand / NmtResetAndWait
    canopen::NmtCommand command { canopen::NmtCommand::EnterPreOperational };

    // LssNodeId / LssBitrate / Readdress
    uint8_t nodeId { 0 };
    canopen::LssBitrate bitrate { canopen::LssBitrate::Rate250k };
    uint32_t bitrateKbps { 0 };

    // The frames this step would put on the wire, built by the same code that
    // will send them -- so what a dry run shows is what an apply does, rather
    // than a second description of it that can drift.
    std::vector<helpers::CanFrame> frames;
};

struct Plan
{
    std::vector<Step> steps;
    // The address the plan starts at, and the one it ends at. Different when
    // the plan reconfigures either.
    uint8_t startNodeId { 0 };
    uint32_t startBitrateKbps { 0 };
    uint8_t endNodeId { 0 };
    uint32_t endBitrateKbps { 0 };
    bool touchesLss { false };
};

// Builds the plan, or explains why it cannot. Reasons include a value outside
// what the EDS declares, a bit rate the device says it does not support, and
// an object the file does not describe.
std::optional<Plan> build_plan(const ReconfigConfig& config, const canopen::ObjectDictionary& od,
                               std::vector<std::string>& errors);

// The plan as text: one line per step, with the frame it would send decoded.
// This is what --dry-run prints.
std::vector<std::string> describe_plan(const Plan& plan);

} // namespace grayhill

#endif // GRAYHILL_RECONFIGURE_PLAN_H
