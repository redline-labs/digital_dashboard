#include <spdlog/spdlog.h>
#include <spdlog/fmt/ranges.h>
#include <cxxopts.hpp>

#include <array>
#include <vector>
#include <thread>
#include <chrono>
#include <algorithm>

#include "pub_sub/zenoh_publisher.h"
#include "pub_sub/zenoh_subscriber.h"
#include "can_frame.capnp.h"
#include "motec_pdm.capnp.h"

#include "dbc_motec_pdm_generic_output_parser.h"

using namespace std::chrono_literals;
using namespace dbc_motec_pdm_generic_output;

// Convert the enum value from the DBC to the Cap'n Proto enum value.
// Enum is expected to be sig_PDM_Output_Status_XX_t::Values
template <typename EnumT>
PdmOutputStatusEnum convert_enum_to_capnp(EnumT input)
{
    switch (input)
    {
        case EnumT::Output_Off_0:
            return PdmOutputStatusEnum::OFF;

        case EnumT::Output_On_1:
            return PdmOutputStatusEnum::ON;

        case EnumT::Output_Fault_Error_2:
            return PdmOutputStatusEnum::FAULT_ERROR;

        case EnumT::Output_Over_Current_Error_4:
            return PdmOutputStatusEnum::OVER_CURRENT_ERROR;

        case EnumT::Output_Retries_Reached_8:
            return PdmOutputStatusEnum::RETRIES_REACHED;

        default:
            return PdmOutputStatusEnum::OFF;
    }
};

int main(int argc, char** argv)
{
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%Y/%m/%d %H:%M:%S.%e%z] [%^%l%$] %v");

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
        auto& out = pubInputState.fields();
        out.setResetSource(static_cast<uint8_t>(m.PDM_Reset_Source));
        out.setRail9v5Volts(static_cast<float>(m.PDM_9V5_Internal_Rail_Voltage));
        out.setTotalCurrentA(static_cast<float>(m.PDM_Total_Current));
        out.setGlobalErrorFlag(static_cast<uint8_t>(m.PDM_Global_Error_Flag));
        out.setBatteryVolts(static_cast<float>(m.PDM_Battery_Voltage));
        out.setInternalTempC(static_cast<float>(m.PDM_Internal_Temperature));

        auto inputs = out.hasInputs() ? out.getInputs() : out.initInputs(24);
        if (inputs.size() != 24) inputs = out.initInputs(24);
        // Map bits PDM_Input_1..23 into list indices 0..22 (simple boolean flags)
        inputs.set(0,  m.PDM_Input_1);
        inputs.set(1,  m.PDM_Input_2);
        inputs.set(2,  m.PDM_Input_3);
        inputs.set(3,  m.PDM_Input_4);
        inputs.set(4,  m.PDM_Input_5);
        inputs.set(5,  m.PDM_Input_6);
        inputs.set(6,  m.PDM_Input_7);
        inputs.set(7,  m.PDM_Input_8);
        inputs.set(8,  m.PDM_Input_9);
        inputs.set(9,  m.PDM_Input_10);
        inputs.set(10, m.PDM_Input_11);
        inputs.set(11, m.PDM_Input_12);
        inputs.set(12, m.PDM_Input_13);
        inputs.set(13, m.PDM_Input_14);
        inputs.set(14, m.PDM_Input_15);
        inputs.set(15, m.PDM_Input_16);
        inputs.set(16, m.PDM_Input_17);
        inputs.set(17, m.PDM_Input_18);
        inputs.set(18, m.PDM_Input_19);
        inputs.set(19, m.PDM_Input_20);
        inputs.set(20, m.PDM_Input_21);
        inputs.set(21, m.PDM_Input_22);
        inputs.set(22, m.PDM_Input_23);
        // element 23 reserved (if needed)
        pubInputState.put();
    });

    parser.on_PDM_Input_Voltage_0x505([&](const PDM_Input_Voltage_0x505_t& m){
        auto& out = pubInfo.fields();
        out.setSerialNumberLow(static_cast<uint8_t>(m.PDM_Serial_Number_Low));
        out.setSerialNumberHigh(static_cast<uint8_t>(m.PDM_Serial_Number_High));
        out.setFwVersionLetter(static_cast<uint8_t>(m.PDM_Firmware_Version_Letter));
        out.setFwVersionMinor(static_cast<uint8_t>(m.PDM_Firmware_Version_Minor));
        out.setFwVersionMajor(static_cast<uint8_t>(m.PDM_Firmware_Version_Major));

        auto& outV = pubInputVoltage.fields();
        constexpr uint32_t count = 23;
        auto list = outV.hasValues() ? outV.getValues() : outV.initValues(count);
        if (list.size() != count) list = outV.initValues(count);
        list.set(0,  static_cast<float>(m.PDM_Input_Voltage_1));
        list.set(1,  static_cast<float>(m.PDM_Input_Voltage_2));
        list.set(2,  static_cast<float>(m.PDM_Input_Voltage_3));
        list.set(3,  static_cast<float>(m.PDM_Input_Voltage_4));
        list.set(4,  static_cast<float>(m.PDM_Input_Voltage_5));
        list.set(5,  static_cast<float>(m.PDM_Input_Voltage_6));
        list.set(6,  static_cast<float>(m.PDM_Input_Voltage_7));
        list.set(7,  static_cast<float>(m.PDM_Input_Voltage_8));
        list.set(8,  static_cast<float>(m.PDM_Input_Voltage_9));
        list.set(9,  static_cast<float>(m.PDM_Input_Voltage_10));
        list.set(10, static_cast<float>(m.PDM_Input_Voltage_11));
        list.set(11, static_cast<float>(m.PDM_Input_Voltage_12));
        list.set(12, static_cast<float>(m.PDM_Input_Voltage_13));
        list.set(13, static_cast<float>(m.PDM_Input_Voltage_14));
        list.set(14, static_cast<float>(m.PDM_Input_Voltage_15));
        list.set(15, static_cast<float>(m.PDM_Input_Voltage_16));
        list.set(16, static_cast<float>(m.PDM_Input_Voltage_17));
        list.set(17, static_cast<float>(m.PDM_Input_Voltage_18));
        list.set(18, static_cast<float>(m.PDM_Input_Voltage_19));
        list.set(19, static_cast<float>(m.PDM_Input_Voltage_20));
        list.set(20, static_cast<float>(m.PDM_Input_Voltage_21));
        list.set(21, static_cast<float>(m.PDM_Input_Voltage_22));
        list.set(22, static_cast<float>(m.PDM_Input_Voltage_23));
        pubInfo.put();
        pubInputVoltage.put();
    });

    parser.on_PDM_Output_Current_0x501([&](const PDM_Output_Current_0x501_t& m){
        auto& out = pubCurrent.fields();
        constexpr uint32_t count = 32;
        auto values = out.hasValues() ? out.getValues() : out.initValues(count);
        if (values.size() != count) values = out.initValues(count);
        values.set(0,  static_cast<float>(m.PDM_Output_Current_1));
        values.set(1,  static_cast<float>(m.PDM_Output_Current_2));
        values.set(2,  static_cast<float>(m.PDM_Output_Current_3));
        values.set(3,  static_cast<float>(m.PDM_Output_Current_4));
        values.set(4,  static_cast<float>(m.PDM_Output_Current_5));
        values.set(5,  static_cast<float>(m.PDM_Output_Current_6));
        values.set(6,  static_cast<float>(m.PDM_Output_Current_7));
        values.set(7,  static_cast<float>(m.PDM_Output_Current_8));
        values.set(8,  static_cast<float>(m.PDM_Output_Current_9));
        values.set(9,  static_cast<float>(m.PDM_Output_Current_10));
        values.set(10, static_cast<float>(m.PDM_Output_Current_11));
        values.set(11, static_cast<float>(m.PDM_Output_Current_12));
        values.set(12, static_cast<float>(m.PDM_Output_Current_13));
        values.set(13, static_cast<float>(m.PDM_Output_Current_14));
        values.set(14, static_cast<float>(m.PDM_Output_Current_15));
        values.set(15, static_cast<float>(m.PDM_Output_Current_16));
        values.set(16, static_cast<float>(m.PDM_Output_Current_17));
        values.set(17, static_cast<float>(m.PDM_Output_Current_18));
        values.set(18, static_cast<float>(m.PDM_Output_Current_19));
        values.set(19, static_cast<float>(m.PDM_Output_Current_20));
        values.set(20, static_cast<float>(m.PDM_Output_Current_21));
        values.set(21, static_cast<float>(m.PDM_Output_Current_22));
        values.set(22, static_cast<float>(m.PDM_Output_Current_23));
        values.set(23, static_cast<float>(m.PDM_Output_Current_24));
        values.set(24, static_cast<float>(m.PDM_Output_Current_25));
        values.set(25, static_cast<float>(m.PDM_Output_Current_26));
        values.set(26, static_cast<float>(m.PDM_Output_Current_27));
        values.set(27, static_cast<float>(m.PDM_Output_Current_28));
        values.set(28, static_cast<float>(m.PDM_Output_Current_29));
        values.set(29, static_cast<float>(m.PDM_Output_Current_30));
        values.set(30, static_cast<float>(m.PDM_Output_Current_31));
        values.set(31, static_cast<float>(m.PDM_Output_Current_32));
        pubCurrent.put();
    });

    parser.on_PDM_Output_Load_0x502([&](const PDM_Output_Load_0x502_t& m){
        auto& out = pubLoad.fields();
        constexpr uint32_t count = 32;
        auto values = out.hasValues() ? out.getValues() : out.initValues(count);
        if (values.size() != count) values = out.initValues(count);
        values.set(0,  static_cast<float>(m.PDM_Output_Load_1));
        values.set(1,  static_cast<float>(m.PDM_Output_Load_2));
        values.set(2,  static_cast<float>(m.PDM_Output_Load_3));
        values.set(3,  static_cast<float>(m.PDM_Output_Load_4));
        values.set(4,  static_cast<float>(m.PDM_Output_Load_5));
        values.set(5,  static_cast<float>(m.PDM_Output_Load_6));
        values.set(6,  static_cast<float>(m.PDM_Output_Load_7));
        values.set(7,  static_cast<float>(m.PDM_Output_Load_8));
        values.set(8,  static_cast<float>(m.PDM_Output_Load_9));
        values.set(9,  static_cast<float>(m.PDM_Output_Load_10));
        values.set(10, static_cast<float>(m.PDM_Output_Load_11));
        values.set(11, static_cast<float>(m.PDM_Output_Load_12));
        values.set(12, static_cast<float>(m.PDM_Output_Load_13));
        values.set(13, static_cast<float>(m.PDM_Output_Load_14));
        values.set(14, static_cast<float>(m.PDM_Output_Load_15));
        values.set(15, static_cast<float>(m.PDM_Output_Load_16));
        values.set(16, static_cast<float>(m.PDM_Output_Load_17));
        values.set(17, static_cast<float>(m.PDM_Output_Load_18));
        values.set(18, static_cast<float>(m.PDM_Output_Load_19));
        values.set(19, static_cast<float>(m.PDM_Output_Load_20));
        values.set(20, static_cast<float>(m.PDM_Output_Load_21));
        values.set(21, static_cast<float>(m.PDM_Output_Load_22));
        values.set(22, static_cast<float>(m.PDM_Output_Load_23));
        values.set(23, static_cast<float>(m.PDM_Output_Load_24));
        values.set(24, static_cast<float>(m.PDM_Output_Load_25));
        values.set(25, static_cast<float>(m.PDM_Output_Load_26));
        values.set(26, static_cast<float>(m.PDM_Output_Load_27));
        values.set(27, static_cast<float>(m.PDM_Output_Load_28));
        values.set(28, static_cast<float>(m.PDM_Output_Load_29));
        values.set(29, static_cast<float>(m.PDM_Output_Load_30));
        values.set(30, static_cast<float>(m.PDM_Output_Load_31));
        values.set(31, static_cast<float>(m.PDM_Output_Load_32));
        pubLoad.put();
    });

    parser.on_PDM_Output_Voltage_0x503([&](const PDM_Output_Voltage_0x503_t& m){
        auto& out = pubVoltage.fields();
        constexpr uint32_t count = 32;
        auto values = out.hasValues() ? out.getValues() : out.initValues(count);
        if (values.size() != count) values = out.initValues(count);
        values.set(0,  static_cast<float>(m.PDM_Output_Voltage_1));
        values.set(1,  static_cast<float>(m.PDM_Output_Voltage_2));
        values.set(2,  static_cast<float>(m.PDM_Output_Voltage_3));
        values.set(3,  static_cast<float>(m.PDM_Output_Voltage_4));
        values.set(4,  static_cast<float>(m.PDM_Output_Voltage_5));
        values.set(5,  static_cast<float>(m.PDM_Output_Voltage_6));
        values.set(6,  static_cast<float>(m.PDM_Output_Voltage_7));
        values.set(7,  static_cast<float>(m.PDM_Output_Voltage_8));
        values.set(8,  static_cast<float>(m.PDM_Output_Voltage_9));
        values.set(9,  static_cast<float>(m.PDM_Output_Voltage_10));
        values.set(10, static_cast<float>(m.PDM_Output_Voltage_11));
        values.set(11, static_cast<float>(m.PDM_Output_Voltage_12));
        values.set(12, static_cast<float>(m.PDM_Output_Voltage_13));
        values.set(13, static_cast<float>(m.PDM_Output_Voltage_14));
        values.set(14, static_cast<float>(m.PDM_Output_Voltage_15));
        values.set(15, static_cast<float>(m.PDM_Output_Voltage_16));
        values.set(16, static_cast<float>(m.PDM_Output_Voltage_17));
        values.set(17, static_cast<float>(m.PDM_Output_Voltage_18));
        values.set(18, static_cast<float>(m.PDM_Output_Voltage_19));
        values.set(19, static_cast<float>(m.PDM_Output_Voltage_20));
        values.set(20, static_cast<float>(m.PDM_Output_Voltage_21));
        values.set(21, static_cast<float>(m.PDM_Output_Voltage_22));
        values.set(22, static_cast<float>(m.PDM_Output_Voltage_23));
        values.set(23, static_cast<float>(m.PDM_Output_Voltage_24));
        values.set(24, static_cast<float>(m.PDM_Output_Voltage_25));
        values.set(25, static_cast<float>(m.PDM_Output_Voltage_26));
        values.set(26, static_cast<float>(m.PDM_Output_Voltage_27));
        values.set(27, static_cast<float>(m.PDM_Output_Voltage_28));
        values.set(28, static_cast<float>(m.PDM_Output_Voltage_29));
        values.set(29, static_cast<float>(m.PDM_Output_Voltage_30));
        values.set(30, static_cast<float>(m.PDM_Output_Voltage_31));
        values.set(31, static_cast<float>(m.PDM_Output_Voltage_32));
        pubVoltage.put();
    });

    parser.on_PDM_Output_Status_0x504([&](const PDM_Output_Status_0x504_t& m){
        auto& out = pubStatus.fields();
        constexpr uint32_t count = 32;
        auto values = out.hasValues() ? out.getValues() : out.initValues(count);
        if (values.size() != count) values = out.initValues(count);
        values.set(0,  convert_enum_to_capnp(m.PDM_Output_Status_1));
        values.set(1,  convert_enum_to_capnp(m.PDM_Output_Status_2));
        values.set(2,  convert_enum_to_capnp(m.PDM_Output_Status_3));
        values.set(3,  convert_enum_to_capnp(m.PDM_Output_Status_4));
        values.set(4,  convert_enum_to_capnp(m.PDM_Output_Status_5));
        values.set(5,  convert_enum_to_capnp(m.PDM_Output_Status_6));
        values.set(6,  convert_enum_to_capnp(m.PDM_Output_Status_7));
        values.set(7,  convert_enum_to_capnp(m.PDM_Output_Status_8));
        values.set(8,  convert_enum_to_capnp(m.PDM_Output_Status_9));
        values.set(9,  convert_enum_to_capnp(m.PDM_Output_Status_10));
        values.set(10, convert_enum_to_capnp(m.PDM_Output_Status_11));
        values.set(11, convert_enum_to_capnp(m.PDM_Output_Status_12));
        values.set(12, convert_enum_to_capnp(m.PDM_Output_Status_13));
        values.set(13, convert_enum_to_capnp(m.PDM_Output_Status_14));
        values.set(14, convert_enum_to_capnp(m.PDM_Output_Status_15));
        values.set(15, convert_enum_to_capnp(m.PDM_Output_Status_16));
        values.set(16, convert_enum_to_capnp(m.PDM_Output_Status_17));
        values.set(17, convert_enum_to_capnp(m.PDM_Output_Status_18));
        values.set(18, convert_enum_to_capnp(m.PDM_Output_Status_19));
        values.set(19, convert_enum_to_capnp(m.PDM_Output_Status_20));
        values.set(20, convert_enum_to_capnp(m.PDM_Output_Status_21));
        values.set(21, convert_enum_to_capnp(m.PDM_Output_Status_22));
        values.set(22, convert_enum_to_capnp(m.PDM_Output_Status_23));
        values.set(23, convert_enum_to_capnp(m.PDM_Output_Status_24));
        values.set(24, convert_enum_to_capnp(m.PDM_Output_Status_25));
        values.set(25, convert_enum_to_capnp(m.PDM_Output_Status_26));
        values.set(26, convert_enum_to_capnp(m.PDM_Output_Status_27));
        values.set(27, convert_enum_to_capnp(m.PDM_Output_Status_28));
        values.set(28, convert_enum_to_capnp(m.PDM_Output_Status_29));
        values.set(29, convert_enum_to_capnp(m.PDM_Output_Status_30));
        values.set(30, convert_enum_to_capnp(m.PDM_Output_Status_31));
        values.set(31, convert_enum_to_capnp(m.PDM_Output_Status_32));
        pubStatus.put();
    });

    // Subscribe to raw CAN frames and feed parser
    pub_sub::ZenohTypedSubscriber<CanFrame> can_subscriber(
        can_key,
        [&parser](CanFrame::Reader frame)
        {
            uint32_t id = frame.getId();
            uint8_t len = frame.getLen();
            auto dataList = frame.getData();
            std::array<uint8_t, 8u> bytes{};
            const size_t n = std::min<size_t>(8u, std::min<size_t>(len, dataList.size()));
            for (size_t i = 0; i < n; ++i)
            {
                bytes[i] = static_cast<uint8_t>(dataList[i]);
            }
            
            parser.handle_can_frame(id, bytes);
        });

    for (;;)
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    return 0;
}


