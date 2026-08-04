// SPDX-License-Identifier: GPL-3.0-or-later
//
// The reconfiguration tool's judgment, which is where its risk lives.
//
// The frames it sends are the easy part; the CANopen runtime already has tests
// for those. What is worth checking here is what the tool decides: that an
// absent key writes nothing, that a value the EDS forbids is refused before it
// reaches the device, that the shorthand and an explicit contradiction of it is
// an error rather than a silent choice, and above all that LSS comes last --
// because a plan that changes the keypad's address before it has finished
// talking to it leaves a device that can only be found by sweeping bit rates.

#include "reconfigure_config.h"
#include "reconfigure_plan.h"

#include "canopen/eds_parser.h"

#include <spdlog/spdlog.h>

#include <fstream>
#include <sstream>

namespace
{

int failures = 0;

void check(bool condition, const std::string& what)
{
    if (!condition)
    {
        SPDLOG_ERROR("FAIL: {}", what);
        ++failures;
    }
}

canopen::ObjectDictionary load_eds()
{
    std::ifstream in(GRAYHILL_EDS_PATH);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    auto result = canopen::parse_eds(buffer.str());
    if (!result.ok())
    {
        SPDLOG_ERROR("the shipped EDS did not parse");
        ++failures;
    }
    return std::move(result.od);
}

std::optional<grayhill::ReconfigConfig> parse(const std::string& yaml,
                                              std::vector<std::string>& errors)
{
    return grayhill::parse_reconfig_config(yaml, errors);
}

// Enough of a config to be valid, so a test can vary one thing at a time.
const char* kMinimal = R"(
current:
  node_id: 0x0A
  bitrate: 250000
)";

// ============================================================================
// Config
// ============================================================================

void test_current_is_required()
{
    std::vector<std::string> errors;
    auto config = parse("target:\n  transmission_type: 0xFE\n", errors);
    check(!config.has_value(), "a config without 'current' is refused");
    check(!errors.empty(), "and says what is missing");

    errors.clear();
    config = parse("current:\n  node_id: 0x0A\n", errors);
    check(!config.has_value(),
          "so is one without a bit rate -- the wrong bit rate is indistinguishable from an "
          "absent keypad, so it is not something to default");
}

void test_absent_means_leave_alone()
{
    std::vector<std::string> errors;
    auto config = parse(std::string(kMinimal) + "target:\n  transmission_type: 0xFE\n", errors);
    check(config.has_value(), "a config with one target key parses");
    if (!config.has_value())
    {
        return;
    }

    check(config->target.transmissionType == 0xFE, "the key that was given is set");
    check(!config->target.backlightScalar.has_value(), "and the one that was not is empty");
    check(!config->target.cobIds.any(), "no COB-ID is implied");
    check(!config->target.nodeId.has_value(), "and no node ID");

    auto od = load_eds();
    errors.clear();
    auto plan = grayhill::build_plan(*config, od, errors);
    check(plan.has_value(), "the plan builds");
    if (!plan.has_value())
    {
        return;
    }

    // Exactly one object is written. This is the property that matters: an
    // absent key must not become a write of a documented default into
    // non-volatile memory.
    size_t writes = 0;
    for (const auto& step : plan->steps)
    {
        if (step.kind == grayhill::Step::Kind::SdoWrite)
        {
            ++writes;
            check(step.index == 0x1800 && step.sub == 2,
                  "the only object written is the one the config named");
        }
    }
    check(writes == 1, "exactly one write, not one per key the tool knows about");
}

void test_motec_shorthand()
{
    std::vector<std::string> errors;
    auto config = parse(std::string(kMinimal) + "target:\n  motec_compatible: true\n", errors);
    check(config.has_value(), "the shorthand parses");
    if (config.has_value())
    {
        check(config->target.transmissionType == 0xFE, "it sets the TPDO1 transmission type");
        check(config->target.backlightScalar == 0xFE, "and the backlight brightness scalar");
    }
}

void test_motec_shorthand_conflict()
{
    std::vector<std::string> errors;
    auto config = parse(
        std::string(kMinimal) + "target:\n  motec_compatible: true\n  backlight_scalar: 0xFF\n",
        errors);
    check(!config.has_value(),
          "asking for the shorthand and contradicting it is an error, not a silent override");
    check(!errors.empty() && errors[0].find("backlight_scalar") != std::string::npos,
          "and the error names the key that disagrees");
}

void test_unknown_key_is_an_error()
{
    std::vector<std::string> errors;
    auto config = parse(std::string(kMinimal) + "target:\n  backlight_scaler: 0xFE\n", errors);
    check(!config.has_value(),
          "a misspelled key is refused rather than ignored -- ignoring it would mean the tool "
          "silently did nothing");
    check(!errors.empty() && errors[0].find("backlight_scaler") != std::string::npos,
          "and the error quotes the misspelling");
}

void test_out_of_range_values()
{
    std::vector<std::string> errors;
    auto config = parse(std::string(kMinimal) + "target:\n  node_id: 300\n", errors);
    check(!config.has_value(), "a node ID of 300 is refused rather than truncated to 44");

    errors.clear();
    config = parse(std::string(kMinimal) + "target:\n  cob_ids:\n    buttons: 0x4000018B\n", errors);
    check(!config.has_value(),
          "a COB-ID pasted back with its control bits is refused: the tool adds those itself");
}

// ============================================================================
// Plan
// ============================================================================

void test_widths_come_from_the_eds()
{
    auto od = load_eds();
    std::vector<std::string> errors;
    auto config = parse(std::string(kMinimal) + "target:\n  motec_compatible: true\n", errors);
    auto plan = grayhill::build_plan(*config, od, errors);
    check(plan.has_value(), "the MoTeC-compatibility plan builds");
    if (!plan.has_value())
    {
        return;
    }

    // The one distinction that decides whether PDM Manager accepts the keypad:
    // 0x1800:02 is one byte, 0x2010:02 is two. Both are read out of the file.
    for (const auto& step : plan->steps)
    {
        if (step.kind != grayhill::Step::Kind::SdoWrite)
        {
            continue;
        }
        if (step.index == 0x1800 && step.sub == 2)
        {
            check(step.width == 1, "0x1800:02 is written as one byte (SDO command byte 0x2F)");
            check(step.frames.front().data[0] == 0x2F, "and the frame says so");
        }
        if (step.index == 0x2010 && step.sub == 2)
        {
            check(step.width == 2, "0x2010:02 is written as two bytes (SDO command byte 0x2B)");
            check(step.frames.front().data[0] == 0x2B, "and the frame says so");
        }
    }
}

void test_plan_refuses_what_the_eds_forbids()
{
    auto od = load_eds();
    std::vector<std::string> errors;

    // 0x2010:02's range is 0x40..0xFF per the manual's Table 1.
    auto config = parse(std::string(kMinimal) + "target:\n  backlight_scalar: 0x10\n", errors);
    check(config.has_value(), "the config itself is well-formed");
    errors.clear();
    auto plan = grayhill::build_plan(*config, od, errors);
    check(!plan.has_value(),
          "a backlight scalar below the declared minimum is refused before the device is "
          "touched");
    check(!errors.empty() && errors[0].find("minimum") != std::string::npos,
          "and the message says which limit it broke");
}

void test_plan_refuses_unsupported_bitrate()
{
    auto od = load_eds();
    std::vector<std::string> errors;
    auto config = parse(std::string(kMinimal) + "target:\n  bitrate: 10000\n", errors);
    check(config.has_value(), "10 kbit/s parses as a number");
    errors.clear();
    auto plan = grayhill::build_plan(*config, od, errors);
    check(!plan.has_value(),
          "but the EDS declares it unsupported, and setting it would leave the keypad "
          "unreachable");
}

void test_lss_comes_last()
{
    auto od = load_eds();
    std::vector<std::string> errors;
    auto config = parse(std::string(kMinimal) + R"(target:
  node_id: 0x0B
  bitrate: 1000000
  motec_compatible: true
  cob_ids:
    buttons: 0x18B
)",
                        errors);
    check(config.has_value(), "a full reconfiguration parses");
    if (!config.has_value())
    {
        for (const auto& message : errors)
        {
            SPDLOG_ERROR("  {}", message);
        }
        return;
    }

    errors.clear();
    auto plan = grayhill::build_plan(*config, od, errors);
    check(plan.has_value(), "and builds");
    if (!plan.has_value())
    {
        for (const auto& message : errors)
        {
            SPDLOG_ERROR("  {}", message);
        }
        return;
    }

    check(plan->touchesLss, "the plan is marked as using LSS");
    check(plan->endNodeId == 0x0B && plan->endBitrateKbps == 1000,
          "and knows where the keypad will end up");

    // Every SDO step must come before every LSS step. This is the ordering
    // rule the whole tool is built around.
    std::optional<size_t> firstLss;
    std::optional<size_t> lastSdo;
    for (size_t i = 0; i < plan->steps.size(); ++i)
    {
        switch (plan->steps[i].kind)
        {
        case grayhill::Step::Kind::LssEnter:
        case grayhill::Step::Kind::LssNodeId:
        case grayhill::Step::Kind::LssBitrate:
        case grayhill::Step::Kind::LssStore:
        case grayhill::Step::Kind::LssExit:
            if (!firstLss.has_value())
            {
                firstLss = i;
            }
            break;
        case grayhill::Step::Kind::SdoWrite:
        case grayhill::Step::Kind::SdoStore:
            lastSdo = i;
            break;
        case grayhill::Step::Kind::NmtCommand:
        case grayhill::Step::Kind::NmtResetAndWait:
        case grayhill::Step::Kind::NmtWaitBootup:
        case grayhill::Step::Kind::Verify:
        case grayhill::Step::Kind::Readdress:
            break;
        }
    }

    check(firstLss.has_value() && lastSdo.has_value(), "the plan has both kinds of step");
    check(firstLss.has_value() && lastSdo.has_value() && *lastSdo < *firstLss,
          "every SDO write happens before the first LSS command -- change your own addressing "
          "last, or you lose the device mid-sequence");

    // The store must come before the reset that makes it matter.
    std::optional<size_t> store;
    std::optional<size_t> sdoReset;
    for (size_t i = 0; i < plan->steps.size(); ++i)
    {
        if (plan->steps[i].kind == grayhill::Step::Kind::SdoStore)
        {
            store = i;
        }
        if (plan->steps[i].kind == grayhill::Step::Kind::NmtResetAndWait && !sdoReset.has_value())
        {
            sdoReset = i;
        }
    }
    check(store.has_value() && sdoReset.has_value() && *store < *sdoReset,
          "the save happens before the reset that would otherwise discard it");

    // The LSS reset, the readdress and the boot-up wait must come in that
    // order. This is the property the tool got wrong first time round: the
    // boot-up frame after an LSS reset arrives at the device's NEW node ID and
    // NEW bit rate, so waiting for it before moving there can never succeed --
    // and on real hardware that looks exactly like a keypad that has been
    // bricked by a half-applied configuration.
    std::optional<size_t> lssExit;
    std::optional<size_t> lssReset;
    std::optional<size_t> readdress;
    std::optional<size_t> waitBootup;
    for (size_t i = 0; i < plan->steps.size(); ++i)
    {
        const auto& step = plan->steps[i];
        if (step.kind == grayhill::Step::Kind::LssExit)
        {
            lssExit = i;
        }
        else if (step.kind == grayhill::Step::Kind::NmtCommand && lssExit.has_value()
                 && step.command == canopen::NmtCommand::ResetNode && !lssReset.has_value())
        {
            lssReset = i;
        }
        else if (step.kind == grayhill::Step::Kind::Readdress)
        {
            readdress = i;
        }
        else if (step.kind == grayhill::Step::Kind::NmtWaitBootup)
        {
            waitBootup = i;
        }
    }

    check(lssReset.has_value(), "the plan resets the device after the LSS store");
    check(readdress.has_value(), "and moves to the new address");
    check(waitBootup.has_value(), "and waits for the boot-up frame");
    check(lssReset.has_value() && readdress.has_value() && *lssReset < *readdress,
          "the reset is sent before the tool moves to the new address");
    check(readdress.has_value() && waitBootup.has_value() && *readdress < *waitBootup,
          "and the tool moves there before waiting for the boot-up, because that frame arrives "
          "at the new node ID and the new bit rate");

    // Nothing may be waited for at the old address after the readdress.
    check(!sdoReset.has_value() || !readdress.has_value() || *sdoReset < *readdress,
          "the SDO reset happens while the device is still at its old address");
}

void test_cobid_is_written_in_two_steps()
{
    auto od = load_eds();
    std::vector<std::string> errors;
    auto config = parse(
        std::string(kMinimal) + "target:\n  cob_ids:\n    buttons: 0x18B\n", errors);
    auto plan = grayhill::build_plan(*config, od, errors);
    check(plan.has_value(), "a COB-ID-only plan builds");
    if (!plan.has_value())
    {
        return;
    }

    std::vector<uint64_t> written;
    for (const auto& step : plan->steps)
    {
        if (step.kind == grayhill::Step::Kind::SdoWrite && step.index == 0x1800 && step.sub == 1)
        {
            written.push_back(step.value);
        }
    }

    check(written.size() == 2, "a COB-ID takes two writes");
    check(written.size() == 2 && written[0] == 0x80000000,
          "the PDO is invalidated first, as CiA requires");
    check(written.size() == 2 && written[1] == (0x40000000u | 0x18B),
          "then given its new identifier with the RTR-not-supported bit set");
}

void test_empty_target_is_an_error()
{
    auto od = load_eds();
    std::vector<std::string> errors;
    auto config = parse(kMinimal, errors);
    check(config.has_value(), "a config with no target parses");
    errors.clear();
    auto plan = grayhill::build_plan(*config, od, errors);
    check(!plan.has_value(),
          "but building a plan from it is an error -- a tool that did nothing and reported "
          "success would be worse than one that says the config is empty");
}

void test_describe_plan_shows_frames()
{
    auto od = load_eds();
    std::vector<std::string> errors;
    auto config = parse(std::string(kMinimal) + "target:\n  motec_compatible: true\n", errors);
    auto plan = grayhill::build_plan(*config, od, errors);
    if (!plan.has_value())
    {
        check(false, "the plan builds");
        return;
    }

    auto lines = grayhill::describe_plan(*plan);
    check(!lines.empty(), "the plan describes itself");

    // The exact frame from the plan document, byte for byte.
    bool foundTxType = false;
    bool foundSave = false;
    for (const auto& line : lines)
    {
        if (line.find("60A [8] 2F 00 18 02 FE 00 00 00") != std::string::npos)
        {
            foundTxType = true;
        }
        if (line.find("60A [8] 23 10 10 01 73 61 76 65") != std::string::npos)
        {
            foundSave = true;
        }
    }
    check(foundTxType, "the dry run shows the transmission-type write byte for byte");
    check(foundSave, "and the \"save\" frame the Grayhill manual documents");
}

} // namespace

int main()
{
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%^%l%$] %v");

    test_current_is_required();
    test_absent_means_leave_alone();
    test_motec_shorthand();
    test_motec_shorthand_conflict();
    test_unknown_key_is_an_error();
    test_out_of_range_values();

    test_widths_come_from_the_eds();
    test_plan_refuses_what_the_eds_forbids();
    test_plan_refuses_unsupported_bitrate();
    test_lss_comes_last();
    test_cobid_is_written_in_two_steps();
    test_empty_target_is_an_error();
    test_describe_plan_shows_frames();

    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }

    SPDLOG_INFO("all reconfiguration checks passed");
    return 0;
}
