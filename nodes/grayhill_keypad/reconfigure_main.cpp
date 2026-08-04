// SPDX-License-Identifier: GPL-3.0-or-later
//
// grayhill_keypad_reconfigure -- change what a Grayhill 3K keypad is, once, and
// then exit.
//
// Deliberately not the runtime node. That one runs forever, speaks PDO only,
// and never touches the object dictionary; this one runs once, speaks SDO, LSS
// and NMT only, and writing non-volatile memory is its entire job. Neither has
// to reason about the other's state because neither does the other's work.
//
// --dry-run is the default and --apply is required to write anything. For a
// tool whose job is mutating non-volatile state on a device that can only be
// recovered by guessing its bit rate, opt-in is the right polarity: the cost of
// an accidental dry run is nothing, and the cost of an accidental apply is a
// bench session with a CAN analyser.

#include "reconfigure_config.h"
#include "reconfigure_plan.h"

#include "canopen/eds_parser.h"
#include "canopen/lss.h"
#include "canopen/nmt.h"
#include "canopen/sdo.h"
#include "canopen/stub_device.h"
#include "canopen/virtual_bus.h"
#include "canopen/zenoh_bus.h"

#include <cxxopts.hpp>
#include <spdlog/spdlog.h>

#include <fstream>
#include <memory>
#include <sstream>

namespace
{

// Exit codes, so a script can tell the failures apart.
constexpr int kOk = 0;
constexpr int kUsageError = 1;
constexpr int kKeypadNotFound = 2;
constexpr int kDeviceRefused = 3;
constexpr int kVerificationMismatch = 4;

std::optional<canopen::ObjectDictionary> load_eds(const std::string& path)
{
    std::ifstream in(path);
    if (!in)
    {
        SPDLOG_ERROR("cannot read the EDS at {}", path);
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();

    auto result = canopen::parse_eds(buffer.str());
    for (const auto& diagnostic : result.diagnostics)
    {
        if (diagnostic.severity == canopen::Severity::Error)
        {
            SPDLOG_ERROR("{}: {}", path, canopen::to_string(diagnostic));
        }
        else
        {
            SPDLOG_WARN("{}: {}", path, canopen::to_string(diagnostic));
        }
    }
    if (!result.ok())
    {
        return std::nullopt;
    }

    for (const auto& problem : canopen::validate(result.od))
    {
        SPDLOG_WARN("{}: {}", path, canopen::to_string(problem));
    }

    return std::move(result.od);
}

// What the tool talks over. The stub is the default because it is the one with
// tests behind it; the zenoh transport reaches a CAN bridge that does not exist
// in this repository yet.
struct Transport
{
    std::unique_ptr<canopen::Bus> bus;
    // Only set for the stub, and only so the summary can say what the
    // simulated device ended up as.
    std::unique_ptr<canopen::StubDevice> keypad;
    canopen::VirtualBus* virtualBus { nullptr };
};

// The simulated keypad sits where it is told to sit, NOT where the config
// claims the keypad is. Building it at the config's address would make
// `current` unfalsifiable -- the tool could never fail to find it, and the most
// common real failure by a wide margin is a `current` that is wrong.
Transport make_stub_transport(const canopen::ObjectDictionary& od,
                              const grayhill::ReconfigConfig& config, uint8_t stubNodeId,
                              uint32_t stubBitrateKbps)
{
    // The bus runs at the rate the *tool* believes it does. A keypad somewhere
    // else simply does not answer, which is what happens on a real bus.
    auto bus = std::make_unique<canopen::VirtualBus>(config.current.bitrateKbps);
    canopen::VirtualBus* raw = bus.get();

    const auto rate = canopen::lss_bitrate_from_kbps(stubBitrateKbps)
                          .value_or(canopen::LssBitrate::Rate250k);

    Transport transport;
    transport.keypad = std::make_unique<canopen::StubDevice>(*raw, od, stubNodeId, rate);
    transport.virtualBus = raw;
    transport.bus = std::move(bus);
    return transport;
}

// Runs the plan. Returns an exit code.
int execute(const grayhill::Plan& plan, Transport& transport, bool singleNodeBus)
{
    canopen::NmtMaster nmt(*transport.bus);
    canopen::SdoClient sdo(*transport.bus, plan.startNodeId);
    canopen::LssMaster lss(*transport.bus, singleNodeBus);

    auto trace = [](const std::string& line) { SPDLOG_INFO("    {}", line); };
    sdo.on_exchange(trace);
    lss.on_exchange(trace);

    nmt.on_emergency([](const canopen::EmcyMessage& message)
                     { SPDLOG_WARN("{}", canopen::to_string(message)); });

    // Before anything is written, prove the keypad is there and answering.
    // Doing this first means "wrong bit rate" and "wrong node ID" -- by far the
    // most common failures -- are reported as themselves rather than as a
    // failure of whatever write happened to come first.
    SPDLOG_INFO("checking that node {} answers", plan.startNodeId);
    auto identity = sdo.upload_u32(0x1018, 1);
    if (!identity.has_value())
    {
        SPDLOG_ERROR("no keypad found at node {} on a {} kbit/s bus: {}", plan.startNodeId,
                     plan.startBitrateKbps, canopen::to_string(identity.error()));
        SPDLOG_ERROR("the usual causes are the wrong bit rate, the wrong node ID, or a bus with "
                     "no CAN bridge terminating it");
        return kKeypadNotFound;
    }
    SPDLOG_INFO("found a device with vendor ID 0x{:04X}", *identity);

    size_t number = 0;
    for (const auto& step : plan.steps)
    {
        SPDLOG_INFO("[{}/{}] {}", ++number, plan.steps.size(), step.description);

        switch (step.kind)
        {
        case grayhill::Step::Kind::NmtCommand:
            nmt.command(step.command, sdo.node_id());
            // NMT is unconfirmed. Give the device a moment to act on it before
            // the next SDO write arrives, or a keypad that is still leaving
            // operational will refuse it.
            transport.bus->poll(canopen::Duration { 20 });
            break;

        case grayhill::Step::Kind::SdoWrite:
        case grayhill::Step::Kind::SdoStore:
        {
            std::vector<uint8_t> bytes(step.width);
            for (uint8_t i = 0; i < step.width; ++i)
            {
                bytes[i] = step.frames.front().data[4 + i];
            }
            auto result = sdo.download(step.index, step.sub, std::move(bytes));
            if (!result.has_value())
            {
                SPDLOG_ERROR("{}", canopen::to_string(result.error()));
                return kDeviceRefused;
            }
            break;
        }

        case grayhill::Step::Kind::NmtResetAndWait:
            if (!nmt.reset_and_wait(sdo.node_id()))
            {
                SPDLOG_ERROR("node {} did not send a boot-up frame after being reset; its state "
                             "is now unknown",
                             sdo.node_id());
                return kDeviceRefused;
            }
            break;

        case grayhill::Step::Kind::NmtWaitBootup:
            if (!nmt.wait_for_bootup(sdo.node_id()))
            {
                SPDLOG_ERROR("node {} did not announce itself after the LSS reset", sdo.node_id());
                SPDLOG_ERROR("the keypad has been written and reset, so it is most likely now at "
                             "node {} on a {} kbit/s bus -- check the interface is running at "
                             "that rate before assuming the worst",
                             plan.endNodeId, plan.endBitrateKbps);
                return kDeviceRefused;
            }
            break;

        case grayhill::Step::Kind::Verify:
        {
            auto value = sdo.upload(step.index, step.sub);
            if (!value.has_value())
            {
                SPDLOG_ERROR("could not read 0x{:04X}:{:02X} back: {}", step.index, step.sub,
                             canopen::to_string(value.error()));
                return kDeviceRefused;
            }
            if (value->as_uint() != step.value)
            {
                SPDLOG_ERROR("0x{:04X}:{:02X} reads 0x{:X} but should be 0x{:X}", step.index,
                             step.sub, value->as_uint(), step.value);
                return kVerificationMismatch;
            }
            // The width matters as much as the value: PDM Manager compares the
            // SDO response command byte for exact equality, so an object
            // served at the wrong width fails its check whatever it holds.
            if (value->expedited && value->size() != step.width)
            {
                SPDLOG_ERROR("0x{:04X}:{:02X} came back as {} byte(s) but the EDS declares {}; a "
                             "tool that checks the SDO response command byte will reject this "
                             "keypad",
                             step.index, step.sub, value->size(), step.width);
                return kVerificationMismatch;
            }
            break;
        }

        case grayhill::Step::Kind::LssEnter:
        {
            auto result = lss.enter_configuration();
            if (!result.has_value())
            {
                SPDLOG_ERROR("{}", canopen::to_string(result.error()));
                SPDLOG_ERROR("pass --single-node-bus once the keypad is the only CANopen device "
                             "on the bus");
                return kUsageError;
            }
            break;
        }

        case grayhill::Step::Kind::LssNodeId:
        {
            auto result = lss.configure_node_id(step.nodeId);
            if (!result.has_value())
            {
                SPDLOG_ERROR("{}", canopen::to_string(result.error()));
                return kDeviceRefused;
            }
            break;
        }

        case grayhill::Step::Kind::LssBitrate:
        {
            auto result = lss.configure_bitrate(step.bitrate);
            if (!result.has_value())
            {
                SPDLOG_ERROR("{}", canopen::to_string(result.error()));
                return kDeviceRefused;
            }
            break;
        }

        case grayhill::Step::Kind::LssStore:
        {
            auto result = lss.store_configuration();
            if (!result.has_value())
            {
                SPDLOG_ERROR("{}", canopen::to_string(result.error()));
                return kDeviceRefused;
            }
            break;
        }

        case grayhill::Step::Kind::LssExit:
        {
            auto result = lss.exit_configuration();
            if (!result.has_value())
            {
                SPDLOG_ERROR("{}", canopen::to_string(result.error()));
                return kDeviceRefused;
            }
            break;
        }

        case grayhill::Step::Kind::Readdress:
            sdo.set_node_id(step.nodeId);
            if (transport.virtualBus != nullptr)
            {
                // A real interface would need its bit rate changed here too.
                // The stub bus models that; the zenoh transport cannot, since
                // the bridge owns the interface.
                transport.virtualBus->set_bitrate_kbps(step.bitrateKbps);
            }
            else
            {
                SPDLOG_WARN("the keypad is now at {} kbit/s -- reconfigure the CAN interface to "
                            "match before talking to it again",
                            step.bitrateKbps);
            }
            break;
        }
    }

    return kOk;
}

} // namespace

int main(int argc, char** argv)
{
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%^%l%$] %v");

    cxxopts::Options options("grayhill_keypad_reconfigure",
                             "Change a Grayhill 3K keypad's configuration, once");
    options.add_options()
        ("config", "Desired-state YAML", cxxopts::value<std::string>())
        ("eds", "Device EDS", cxxopts::value<std::string>()->default_value(GRAYHILL_EDS_PATH))
        ("transport", "stub or zenoh", cxxopts::value<std::string>()->default_value("stub"))
        ("apply", "Actually write. Without this the tool prints the plan and exits",
         cxxopts::value<bool>()->default_value("false")->implicit_value("true"))
        ("dry-run", "Print the plan and exit. The default",
         cxxopts::value<bool>()->default_value("true")->implicit_value("true"))
        ("single-node-bus",
         "Assert that the keypad is the only CANopen device on the bus. Required for LSS",
         cxxopts::value<bool>()->default_value("false")->implicit_value("true"))
        // Where the simulated keypad is. The defaults are Grayhill's factory
        // settings from the manual's Table 1, so `--transport stub` models a
        // keypad straight out of the box -- including being unreachable if
        // `current` in the config says something else.
        ("stub-node-id", "Node ID of the simulated keypad",
         cxxopts::value<int>()->default_value("10"))
        ("stub-bitrate", "Bit rate of the simulated keypad, in bit/s",
         cxxopts::value<int>()->default_value("250000"))
        ("tx-key", "Zenoh key for frames out",
         cxxopts::value<std::string>()->default_value("vehicle/can0/tx"))
        ("rx-key", "Zenoh key for frames in",
         cxxopts::value<std::string>()->default_value("vehicle/can0/rx"))
        ("h,help", "Print usage");

    cxxopts::ParseResult args;
    try
    {
        args = options.parse(argc, argv);
    }
    catch (const std::exception& error)
    {
        SPDLOG_ERROR("{}", error.what());
        return kUsageError;
    }

    if (args.count("help") != 0)
    {
        SPDLOG_INFO("{}", options.help());
        return kOk;
    }

    if (args.count("config") == 0)
    {
        SPDLOG_ERROR("--config is required");
        SPDLOG_INFO("{}", options.help());
        return kUsageError;
    }

    // --- what to do ---------------------------------------------------------
    std::vector<std::string> errors;
    auto config = grayhill::load_reconfig_config(args["config"].as<std::string>(), errors);
    if (!config.has_value())
    {
        for (const auto& message : errors)
        {
            SPDLOG_ERROR("{}: {}", args["config"].as<std::string>(), message);
        }
        return kUsageError;
    }

    auto od = load_eds(args["eds"].as<std::string>());
    if (!od.has_value())
    {
        return kUsageError;
    }

    auto plan = grayhill::build_plan(*config, *od, errors);
    if (!plan.has_value())
    {
        for (const auto& message : errors)
        {
            SPDLOG_ERROR("{}", message);
        }
        return kUsageError;
    }

    // --- show it ------------------------------------------------------------
    for (const auto& line : grayhill::describe_plan(*plan))
    {
        SPDLOG_INFO("{}", line);
    }

    // `--dry-run` wins over `--apply` when both are given. The two contradict
    // each other, and for a tool that writes non-volatile memory the safe
    // reading of a contradictory command line is the one that does nothing.
    const bool explicitDryRun = args.count("dry-run") != 0;
    const bool apply = args["apply"].as<bool>() && !explicitDryRun;
    if (args["apply"].as<bool>() && explicitDryRun)
    {
        SPDLOG_WARN("--dry-run and --apply contradict each other; doing the dry run");
    }

    if (!apply)
    {
        SPDLOG_INFO("");
        SPDLOG_INFO("dry run: nothing was sent. Pass --apply to write this to the keypad.");
        return kOk;
    }

    const bool singleNodeBus = args["single-node-bus"].as<bool>();
    if (plan->touchesLss && !singleNodeBus)
    {
        SPDLOG_ERROR("this plan uses LSS, which broadcasts to every CANopen device on the bus.");
        SPDLOG_ERROR("Disconnect everything but the keypad and the gateway, then pass "
                     "--single-node-bus.");
        return kUsageError;
    }

    // --- do it --------------------------------------------------------------
    const std::string transportName = args["transport"].as<std::string>();
    Transport transport;

    if (transportName == "stub")
    {
        const uint8_t stubNodeId = static_cast<uint8_t>(args["stub-node-id"].as<int>());
        const uint32_t stubBitrate = static_cast<uint32_t>(args["stub-bitrate"].as<int>()) / 1000;

        SPDLOG_INFO("");
        SPDLOG_INFO("applying against a simulated keypad at node {} on a {} kbit/s bus "
                    "(--transport stub); no CAN traffic leaves this process",
                    stubNodeId, stubBitrate);
        transport = make_stub_transport(*od, *config, stubNodeId, stubBitrate);
    }
    else if (transportName == "zenoh")
    {
        SPDLOG_INFO("");
        SPDLOG_INFO("applying over zenoh: frames out on '{}', frames in on '{}'",
                    args["tx-key"].as<std::string>(), args["rx-key"].as<std::string>());
        SPDLOG_WARN("nothing in this repository terminates those keys yet -- a CAN bridge node "
                    "has to be running for this to reach hardware");

        auto bus = std::make_unique<canopen::ZenohBus>(args["tx-key"].as<std::string>(),
                                                       args["rx-key"].as<std::string>());
        if (!bus->is_valid())
        {
            SPDLOG_ERROR("could not open a zenoh session; nothing would be sent");
            return kUsageError;
        }
        transport.bus = std::move(bus);
    }
    else
    {
        SPDLOG_ERROR("unknown transport '{}'; expected 'stub' or 'zenoh'", transportName);
        return kUsageError;
    }

    const int result = execute(*plan, transport, singleNodeBus);

    if (result == kOk)
    {
        SPDLOG_INFO("");
        SPDLOG_INFO("done: the keypad is node {} at {} kbit/s, and every write was read back",
                    plan->endNodeId, plan->endBitrateKbps);
        if (transport.keypad != nullptr)
        {
            SPDLOG_INFO("(this was the simulated keypad; nothing on a real bus was touched)");
        }
    }

    return result;
}
