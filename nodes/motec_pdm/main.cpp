#include <span>
#include "core/core.h"
#include <spdlog/spdlog.h>
#include <spdlog/fmt/ranges.h>
#include <cxxopts.hpp>

#include <array>
#include <vector>
#include <chrono>
#include <algorithm>

#include "cli/interrupt.h"
#include "node_health/can_decoder.h"
#include "pub_sub/node_identity.h"
#include "pub_sub/zenoh_publisher.h"
#include "motec_pdm.capnp.h"

#include "dbc_motec_pdm_generic_output_parser.h"
#include "pdm_messages.h"

using namespace std::chrono_literals;
using namespace dbc_motec_pdm_generic_output;

int main(int argc, char** argv)
{
    core::setupLogging({.program = "motec_pdm"});

    cxxopts::Options options("motec_pdm", "Decode PDM_Generic_Output.dbc frames and publish typed telemetry");
    options.add_options()
        ("s,source", "Zenoh key to subscribe to CAN frames", cxxopts::value<std::string>()->default_value("vehicle/can0/rx"))
        ("p,prefix", "Zenoh key prefix for PDM topics", cxxopts::value<std::string>()->default_value("nodes/motec_pdm"))
        ("h,help", "Print usage");

    auto result = options.parse(argc, argv);
    if (result.count("help"))
    {
        SPDLOG_INFO("{}", options.help());
        return 0;
    }

    const std::string can_key = result["source"].as<std::string>();
    const std::string prefix  = result["prefix"].as<std::string>();

    // Announce this process so tools can put a name to the session id that
    // appears on every topic it advertises and every sample it stamps. See
    // pub_sub/node_identity.h.
    pub_sub::NodeIdentity node_identity("motec_pdm");

    // Early, so a SIGTERM during startup still ends in a clean exit.
    cli::installInterruptHandler();

    SPDLOG_INFO("Subscribing to CAN frames on key '{}'", can_key);

    // Publishers for each logical group
    pub_sub::ZenohPublisher<MotecPdmInputState>     pubInputState(prefix + "/input_state");
    pub_sub::ZenohPublisher<MotecPdmInfo>           pubInfo(prefix + "/info");
    pub_sub::ZenohPublisher<MotecPdmOutputCurrent>  pubCurrent(prefix + "/output_current");
    pub_sub::ZenohPublisher<MotecPdmOutputLoad>     pubLoad(prefix + "/output_load");
    pub_sub::ZenohPublisher<MotecPdmOutputVoltage>  pubVoltage(prefix + "/output_voltage");
    pub_sub::ZenohPublisher<MotecPdmOutputStatus>   pubStatus(prefix + "/output_status");
    pub_sub::ZenohPublisher<MotecPdmInputVoltage>   pubInputVoltage(prefix + "/input_voltage");

    SPDLOG_INFO("Publishing MotecPdmInputState on key '{}'", pubInputState.keyexpr());
    SPDLOG_INFO("Publishing MotecPdmInfo on key '{}'", pubInfo.keyexpr());
    SPDLOG_INFO("Publishing MotecPdmOutputCurrent on key '{}'", pubCurrent.keyexpr());
    SPDLOG_INFO("Publishing MotecPdmOutputLoad on key '{}'", pubLoad.keyexpr());
    SPDLOG_INFO("Publishing MotecPdmOutputVoltage on key '{}'", pubVoltage.keyexpr());
    SPDLOG_INFO("Publishing MotecPdmOutputStatus on key '{}'", pubStatus.keyexpr());
    SPDLOG_INFO("Publishing MotecPdmInputVoltage on key '{}'", pubInputVoltage.keyexpr());

    dbc_motec_pdm_generic_output_parser parser;

    parser.on_PDM_Input_State_0x500([&](const PDM_Input_State_0x500_t& m){
        motec_pdm::fillInputState(m, pubInputState.fields());
        pubInputState.put();
    });

    parser.on_PDM_Input_Voltage_0x505([&](const PDM_Input_Voltage_0x505_t& m){
        motec_pdm::fillInfo(m, pubInfo.fields());
        motec_pdm::fillInputVoltage(m, pubInputVoltage.fields());
        pubInfo.put();
        pubInputVoltage.put();
    });

    parser.on_PDM_Output_Current_0x501([&](const PDM_Output_Current_0x501_t& m){
        motec_pdm::fillOutputCurrent(m, pubCurrent.fields());
        pubCurrent.put();
    });

    parser.on_PDM_Output_Load_0x502([&](const PDM_Output_Load_0x502_t& m){
        motec_pdm::fillOutputLoad(m, pubLoad.fields());
        pubLoad.put();
    });

    parser.on_PDM_Output_Voltage_0x503([&](const PDM_Output_Voltage_0x503_t& m){
        motec_pdm::fillOutputVoltage(m, pubVoltage.fields());
        pubVoltage.put();
    });

    parser.on_PDM_Output_Status_0x504([&](const PDM_Output_Status_0x504_t& m){
        motec_pdm::fillOutputStatus(m, pubStatus.fields());
        pubStatus.put();
    });

    // Health, the CAN subscription and the wait for SIGTERM: the same in
    // every decoder node.
    node_health::runCanDecoder("motec_pdm", can_key, [&parser](const helpers::CanFrame& frame) {
        return parser.handle_can_frame(frame.id, frame.data_span());
    });
    return 0;
}


