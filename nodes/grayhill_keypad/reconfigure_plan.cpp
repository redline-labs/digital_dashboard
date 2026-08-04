// SPDX-License-Identifier: GPL-3.0-or-later

#include "reconfigure_plan.h"

#include <spdlog/fmt/fmt.h>

namespace grayhill
{
namespace
{

// CiA's two-step COB-ID write: invalidate the PDO first, then give it its new
// identifier with the "RTR not supported" bit set. Writing the new value in one
// step is not allowed while the PDO is valid, and the Grayhill manual and PDM
// Manager both do it this way.
constexpr uint32_t kCobIdInvalid = 0x80000000;
constexpr uint32_t kCobIdRtrDisabled = 0x40000000;

// How wide the device serves an object, taken from the file. A tool that
// guessed would be guessing at the one thing this device is fussy about.
std::optional<uint8_t> width_of(const canopen::ObjectDictionary& od, uint16_t index, uint8_t sub,
                                std::vector<std::string>& errors)
{
    const canopen::SubObject* entry = od.get(index, sub);
    if (entry == nullptr)
    {
        errors.push_back(fmt::format(
            "the EDS does not describe 0x{:04X}:{:02X}, so the tool cannot know how wide to "
            "write it",
            index, sub));
        return std::nullopt;
    }

    auto bits = canopen::data_type_bits(entry->dataType);
    if (!bits.has_value() || *bits > 32)
    {
        errors.push_back(fmt::format("0x{:04X}:{:02X} is {}, which an expedited SDO cannot carry",
                                     index, sub, canopen::to_string(entry->dataType)));
        return std::nullopt;
    }

    return static_cast<uint8_t>((*bits + 7) / 8);
}

// Refuses a value the file says the device will not accept. The device would
// abort anyway; finding out before touching it is better.
bool check_limits(const canopen::ObjectDictionary& od, uint16_t index, uint8_t sub, uint64_t value,
                  const char* what, std::vector<std::string>& errors)
{
    const canopen::SubObject* entry = od.get(index, sub);
    if (entry == nullptr)
    {
        return true;
    }

    const int64_t asSigned = static_cast<int64_t>(value);
    if (entry->lowLimit.has_value() && asSigned < *entry->lowLimit)
    {
        errors.push_back(fmt::format("{} ({}) is below the {} minimum of {} that the EDS declares",
                                     what, value, entry->parameterName, *entry->lowLimit));
        return false;
    }
    if (entry->highLimit.has_value() && asSigned > *entry->highLimit)
    {
        errors.push_back(fmt::format("{} ({}) is above the {} maximum of {} that the EDS declares",
                                     what, value, entry->parameterName, *entry->highLimit));
        return false;
    }
    if (!canopen::is_writable(entry->access))
    {
        errors.push_back(fmt::format("{} is {} in the EDS and cannot be written", what,
                                     canopen::to_string(entry->access)));
        return false;
    }
    return true;
}

std::vector<uint8_t> to_bytes(uint64_t value, uint8_t width)
{
    std::vector<uint8_t> bytes(width);
    for (uint8_t i = 0; i < width; ++i)
    {
        bytes[i] = static_cast<uint8_t>((value >> (8 * i)) & 0xFF);
    }
    return bytes;
}

class PlanBuilder
{
public:
    PlanBuilder(const canopen::ObjectDictionary& od, uint8_t nodeId,
                std::vector<std::string>& errors)
        : od_(od)
        , nodeId_(nodeId)
        , errors_(errors)
    {
    }

    void nmt(canopen::NmtCommand command, std::string description)
    {
        Step step;
        step.kind = Step::Kind::NmtCommand;
        step.command = command;
        step.description = std::move(description);
        step.frames.push_back(canopen::make_nmt_frame(command, nodeId_));
        plan_.steps.push_back(std::move(step));
    }

    void reset_and_wait()
    {
        Step step;
        step.kind = Step::Kind::NmtResetAndWait;
        step.command = canopen::NmtCommand::ResetNode;
        step.nodeId = nodeId_;
        step.description
            = fmt::format("reset node {} and wait for its boot-up frame on 0x{:03X}", nodeId_,
                          canopen::NmtMaster::kHeartbeatCobIdBase + nodeId_);
        step.frames.push_back(canopen::make_nmt_frame(canopen::NmtCommand::ResetNode, nodeId_));
        plan_.steps.push_back(std::move(step));
    }

    // One SDO write, with the width taken from the file.
    bool write(uint16_t index, uint8_t sub, uint64_t value, const std::string& what)
    {
        auto width = width_of(od_, index, sub, errors_);
        if (!width.has_value())
        {
            return false;
        }
        if (!check_limits(od_, index, sub, value, what.c_str(), errors_))
        {
            return false;
        }

        Step step;
        step.kind = Step::Kind::SdoWrite;
        step.index = index;
        step.sub = sub;
        step.value = value;
        step.width = *width;
        step.description = fmt::format("{}: 0x{:04X}:{:02X} <- 0x{:0{}X} ({} byte{})", what, index,
                                       sub, value, *width * 2, *width, *width == 1 ? "" : "s");
        step.frames.push_back(
            canopen::make_sdo_download_frame(nodeId_, index, sub, to_bytes(value, *width)));
        plan_.steps.push_back(std::move(step));
        return true;
    }

    // A COB-ID, written the way CiA requires.
    bool write_cobid(uint16_t index, uint32_t cobid, const std::string& what)
    {
        auto width = width_of(od_, index, 1, errors_);
        if (!width.has_value())
        {
            return false;
        }
        if (*width != 4)
        {
            errors_.push_back(fmt::format("0x{:04X}:01 is {} bytes; a COB-ID is four", index,
                                          *width));
            return false;
        }

        Step invalidate;
        invalidate.kind = Step::Kind::SdoWrite;
        invalidate.index = index;
        invalidate.sub = 1;
        invalidate.value = kCobIdInvalid;
        invalidate.width = 4;
        invalidate.description
            = fmt::format("{}: invalidate the PDO first (0x{:04X}:01 <- 0x80000000), because CiA "
                          "does not allow changing a valid PDO's COB-ID",
                          what, index);
        invalidate.frames.push_back(
            canopen::make_sdo_download_frame(nodeId_, index, 1, to_bytes(kCobIdInvalid, 4)));
        plan_.steps.push_back(std::move(invalidate));

        const uint32_t encoded = kCobIdRtrDisabled | cobid;
        Step assign;
        assign.kind = Step::Kind::SdoWrite;
        assign.index = index;
        assign.sub = 1;
        assign.value = encoded;
        assign.width = 4;
        assign.description = fmt::format("{}: 0x{:04X}:01 <- 0x{:08X} (COB-ID 0x{:03X}, valid, RTR "
                                         "not supported)",
                                         what, index, encoded, cobid);
        assign.frames.push_back(
            canopen::make_sdo_download_frame(nodeId_, index, 1, to_bytes(encoded, 4)));
        plan_.steps.push_back(std::move(assign));
        return true;
    }

    void store()
    {
        Step step;
        step.kind = Step::Kind::SdoStore;
        step.index = 0x1010;
        step.sub = 1;
        step.width = 4;
        step.description = "store parameters: 0x1010:01 <- \"save\" -- without this nothing "
                           "above survives the next power cycle";
        const std::vector<uint8_t> save { 's', 'a', 'v', 'e' };
        step.frames.push_back(canopen::make_sdo_download_frame(nodeId_, 0x1010, 1, save));
        plan_.steps.push_back(std::move(step));
    }

    void verify(uint16_t index, uint8_t sub, uint64_t expected, const std::string& what)
    {
        auto width = width_of(od_, index, sub, errors_);
        if (!width.has_value())
        {
            return;
        }

        Step step;
        step.kind = Step::Kind::Verify;
        step.index = index;
        step.sub = sub;
        step.value = expected;
        step.width = *width;
        step.description = fmt::format("read back 0x{:04X}:{:02X} and expect 0x{:0{}X} ({})", index,
                                       sub, expected, *width * 2, what);
        step.frames.push_back(canopen::make_sdo_upload_frame(nodeId_, index, sub));
        plan_.steps.push_back(std::move(step));
    }

    void lss_enter()
    {
        Step step;
        step.kind = Step::Kind::LssEnter;
        step.description = "LSS: switch every device on the bus into configuration mode -- this "
                           "is a global broadcast, which is why the bus must hold only the keypad";
        step.frames.push_back(canopen::make_lss_frame(canopen::lss_cs::kSwitchStateGlobal,
                                                      canopen::lss_cs::kModeConfiguration));
        plan_.steps.push_back(std::move(step));
    }

    void lss_node_id(uint8_t nodeId)
    {
        Step step;
        step.kind = Step::Kind::LssNodeId;
        step.nodeId = nodeId;
        step.description = fmt::format("LSS: configure node ID {} (takes effect on the next reset)",
                                       nodeId);
        step.frames.push_back(
            canopen::make_lss_frame(canopen::lss_cs::kConfigureNodeId, nodeId));
        plan_.steps.push_back(std::move(step));
    }

    void lss_bitrate(canopen::LssBitrate rate)
    {
        Step step;
        step.kind = Step::Kind::LssBitrate;
        step.bitrate = rate;
        step.bitrateKbps = canopen::lss_bitrate_to_kbps(rate);
        step.description
            = fmt::format("LSS: configure bit timing to {} (takes effect on the next reset)",
                          canopen::to_string(rate));
        step.frames.push_back(canopen::make_lss_frame(canopen::lss_cs::kConfigureBitTiming,
                                                      canopen::lss_cs::kStandardBitTimingTable,
                                                      static_cast<uint8_t>(rate)));
        plan_.steps.push_back(std::move(step));
    }

    void lss_store()
    {
        Step step;
        step.kind = Step::Kind::LssStore;
        step.description = "LSS: store the configuration -- node ID and bit rate persist through "
                           "this, not through the SDO save above";
        step.frames.push_back(canopen::make_lss_frame(canopen::lss_cs::kStoreConfiguration, 0));
        plan_.steps.push_back(std::move(step));
    }

    void lss_exit()
    {
        Step step;
        step.kind = Step::Kind::LssExit;
        step.description = "LSS: return the bus to waiting mode";
        step.frames.push_back(canopen::make_lss_frame(canopen::lss_cs::kSwitchStateGlobal,
                                                      canopen::lss_cs::kModeWaiting));
        plan_.steps.push_back(std::move(step));
    }

    void wait_for_bootup()
    {
        Step step;
        step.kind = Step::Kind::NmtWaitBootup;
        step.nodeId = nodeId_;
        step.description = fmt::format(
            "wait for node {}'s boot-up frame on 0x{:03X} -- it arrives at the new address, not "
            "the old one",
            nodeId_, canopen::NmtMaster::kHeartbeatCobIdBase + nodeId_);
        plan_.steps.push_back(std::move(step));
    }

    // Everything after this addresses the device differently.
    void readdress(uint8_t nodeId, uint32_t bitrateKbps)
    {
        Step step;
        step.kind = Step::Kind::Readdress;
        step.nodeId = nodeId;
        step.bitrateKbps = bitrateKbps;
        step.description = fmt::format(
            "from here on the keypad is node {} at {} kbit/s; everything before this used node "
            "{} at {} kbit/s",
            nodeId, bitrateKbps, nodeId_, plan_.startBitrateKbps);
        plan_.steps.push_back(std::move(step));
        nodeId_ = nodeId;
    }

    Plan& plan() { return plan_; }
    uint8_t node_id() const { return nodeId_; }

private:
    const canopen::ObjectDictionary& od_;
    uint8_t nodeId_;
    std::vector<std::string>& errors_;
    Plan plan_;
};

} // namespace

std::optional<Plan> build_plan(const ReconfigConfig& config, const canopen::ObjectDictionary& od,
                               std::vector<std::string>& errors)
{
    PlanBuilder builder(od, config.current.nodeId, errors);
    Plan& plan = builder.plan();

    plan.startNodeId = config.current.nodeId;
    plan.startBitrateKbps = config.current.bitrateKbps;
    plan.endNodeId = config.target.nodeId.value_or(config.current.nodeId);
    plan.endBitrateKbps = config.target.bitrateKbps.value_or(config.current.bitrateKbps);

    // --- 1. everything that goes over SDO, at the current address -----------
    //
    // Communication parameters can only be changed in pre-operational, so that
    // comes first whatever else is being written.
    const bool anySdoWrite = config.target.cobIds.any() || config.target.heartbeatMs.has_value()
        || config.target.eventTimerMs.has_value() || config.target.inhibitTimeUs.has_value()
        || config.target.transmissionType.has_value()
        || config.target.indicatorScalar.has_value() || config.target.backlightScalar.has_value();

    if (anySdoWrite)
    {
        builder.nmt(canopen::NmtCommand::EnterPreOperational,
                    "put the keypad in pre-operational: communication parameters cannot be "
                    "changed while it is operational");
    }

    if (config.target.cobIds.buttons.has_value())
    {
        builder.write_cobid(0x1800, *config.target.cobIds.buttons, "buttons (TPDO1)");
    }
    if (config.target.cobIds.indicators.has_value())
    {
        builder.write_cobid(0x1400, *config.target.cobIds.indicators, "indicators (RPDO1)");
    }
    if (config.target.cobIds.brightness.has_value())
    {
        builder.write_cobid(0x1401, *config.target.cobIds.brightness, "brightness (RPDO2)");
    }
    if (config.target.heartbeatMs.has_value())
    {
        builder.write(0x1017, 0, *config.target.heartbeatMs, "producer heartbeat time");
    }
    if (config.target.transmissionType.has_value())
    {
        builder.write(0x1800, 2, *config.target.transmissionType, "TPDO1 transmission type");
    }
    if (config.target.inhibitTimeUs.has_value())
    {
        builder.write(0x1800, 3, *config.target.inhibitTimeUs, "TPDO1 inhibit time");
    }
    if (config.target.eventTimerMs.has_value())
    {
        builder.write(0x1800, 5, *config.target.eventTimerMs, "TPDO1 event timer");
    }
    if (config.target.indicatorScalar.has_value())
    {
        builder.write(0x2010, 1, *config.target.indicatorScalar, "indicator brightness scalar");
    }
    if (config.target.backlightScalar.has_value())
    {
        builder.write(0x2010, 2, *config.target.backlightScalar, "backlight brightness scalar");
    }

    // --- 2. persist ---------------------------------------------------------
    if (config.store && anySdoWrite)
    {
        builder.store();
    }

    // --- 3. reset, and check the writes survived it -------------------------
    if (config.resetAfter && anySdoWrite)
    {
        builder.reset_and_wait();

        // Reading back after the reset is what distinguishes "the device
        // accepted the write" from "the write is still there", which is the
        // question the Store step exists to answer.
        if (config.target.transmissionType.has_value())
        {
            builder.verify(0x1800, 2, *config.target.transmissionType,
                           "TPDO1 transmission type");
        }
        if (config.target.backlightScalar.has_value())
        {
            builder.verify(0x2010, 2, *config.target.backlightScalar,
                           "backlight brightness scalar");
        }
        if (config.target.indicatorScalar.has_value())
        {
            builder.verify(0x2010, 1, *config.target.indicatorScalar,
                           "indicator brightness scalar");
        }
        if (config.target.heartbeatMs.has_value())
        {
            builder.verify(0x1017, 0, *config.target.heartbeatMs, "producer heartbeat time");
        }
        if (config.target.eventTimerMs.has_value())
        {
            builder.verify(0x1800, 5, *config.target.eventTimerMs, "TPDO1 event timer");
        }
        if (config.target.inhibitTimeUs.has_value())
        {
            builder.verify(0x1800, 3, *config.target.inhibitTimeUs, "TPDO1 inhibit time");
        }
        if (config.target.cobIds.buttons.has_value())
        {
            builder.verify(0x1800, 1, kCobIdRtrDisabled | *config.target.cobIds.buttons,
                           "buttons COB-ID");
        }
        if (config.target.cobIds.indicators.has_value())
        {
            builder.verify(0x1400, 1, kCobIdRtrDisabled | *config.target.cobIds.indicators,
                           "indicators COB-ID");
        }
        if (config.target.cobIds.brightness.has_value())
        {
            builder.verify(0x1401, 1, kCobIdRtrDisabled | *config.target.cobIds.brightness,
                           "brightness COB-ID");
        }
    }

    // --- 4. LSS, last -------------------------------------------------------
    const bool changingNodeId
        = config.target.nodeId.has_value() && *config.target.nodeId != config.current.nodeId;
    const bool changingBitrate = config.target.bitrateKbps.has_value()
        && *config.target.bitrateKbps != config.current.bitrateKbps;

    if (changingNodeId || changingBitrate)
    {
        plan.touchesLss = true;

        std::optional<canopen::LssBitrate> rate;
        if (changingBitrate)
        {
            rate = canopen::lss_bitrate_from_kbps(*config.target.bitrateKbps);
            if (!rate.has_value())
            {
                errors.push_back(fmt::format(
                    "{} kbit/s is not one of the rates LSS can select", *config.target.bitrateKbps));
            }
            else
            {
                // The EDS says which rates the device implements. Asking for
                // one it does not is how a keypad becomes unreachable.
                auto declared
                    = od.deviceInfo.supportedBitrates.find(*config.target.bitrateKbps);
                if (declared != od.deviceInfo.supportedBitrates.end() && !declared->second)
                {
                    errors.push_back(fmt::format(
                        "the EDS declares {} kbit/s unsupported on this device; setting it would "
                        "leave the keypad unreachable",
                        *config.target.bitrateKbps));
                }
            }
        }

        if (!od.deviceInfo.lssSupported)
        {
            errors.push_back("the EDS says this device does not support LSS, so its node ID and "
                             "bit rate cannot be changed this way");
        }

        builder.lss_enter();
        if (changingNodeId)
        {
            builder.lss_node_id(*config.target.nodeId);
        }
        if (changingBitrate && rate.has_value())
        {
            builder.lss_bitrate(*rate);
        }
        builder.lss_store();
        builder.lss_exit();

        // The reset that adopts the LSS configuration is split from the wait
        // for its boot-up frame, because between the two the device changes
        // address. Sending the reset and then waiting at the old node ID and
        // the old bit rate can never succeed: the boot-up goes out at the new
        // ones, and on a real bus a client still at the old rate cannot even
        // see the bits.
        builder.nmt(canopen::NmtCommand::ResetNode,
                    fmt::format("reset node {} so it adopts the LSS configuration",
                                builder.node_id()));
        builder.readdress(plan.endNodeId, plan.endBitrateKbps);
        builder.wait_for_bootup();

        // One read at the new address, to prove the device is still there.
        builder.verify(0x1018, 1, 0x0307, "vendor ID, read at the new address");
    }

    if (plan.steps.empty())
    {
        errors.push_back("the config asks for nothing: every target key is absent, and an absent "
                         "key means leave the device alone");
    }

    if (!errors.empty())
    {
        return std::nullopt;
    }
    return plan;
}

std::vector<std::string> describe_plan(const Plan& plan)
{
    std::vector<std::string> lines;

    lines.push_back(fmt::format("keypad at node {} ({} kbit/s)", plan.startNodeId,
                                plan.startBitrateKbps));
    if (plan.endNodeId != plan.startNodeId || plan.endBitrateKbps != plan.startBitrateKbps)
    {
        lines.push_back(fmt::format("  -> will end up at node {} ({} kbit/s)", plan.endNodeId,
                                    plan.endBitrateKbps));
    }
    lines.push_back("");

    size_t number = 0;
    for (const auto& step : plan.steps)
    {
        lines.push_back(fmt::format("{:2}. {}", ++number, step.description));
        for (const auto& frame : step.frames)
        {
            lines.push_back(fmt::format("      {:03X} [{}] {}", frame.id, frame.len,
                                        canopen::format_frame_data(frame)));
        }
    }

    return lines;
}

} // namespace grayhill
