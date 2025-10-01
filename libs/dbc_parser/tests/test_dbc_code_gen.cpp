#include "dbc_motec_pdm_generic_output.h"
#include <spdlog/spdlog.h>
#include <spdlog/fmt/ranges.h>

#include <map>
#include <string>
#include <cmath>

bool close_enough(double a, double b)
{
    return std::abs(a - b) < 1e-6;
}

int main()
{
    spdlog::set_level(spdlog::level::debug);
    spdlog::set_pattern("[%Y/%m/%d %H:%M:%S.%e%z] [%^%l%$] [%t:%s:%#] %v");

    std::map<std::string, double> expected_results = {
        {"PDM_Input_Voltage_1", 1.0},
        {"PDM_Input_Voltage_2", 2.0},
        {"PDM_Input_Voltage_3", 3.0},
        {"PDM_Input_Voltage_4", 4.0},
        {"PDM_Input_Voltage_5", 5.0},
        {"PDM_Input_Voltage_6", 6.0},
        {"PDM_Input_Voltage_7", 7.0},
        {"PDM_Input_Voltage_MP", 0.0},
    };

    dbc_motec_pdm_generic_output::dbc_motec_pdm_generic_output_t db;
    std::array<uint8_t, 8> data = {0x00, 0x05, 0x0a, 0x0f, 0x14, 0x19, 0x1e, 0x23};
    auto decoded = db.decode(0x000505, data);
    SPDLOG_INFO("decoded = {}", db.get_message_name(decoded));

    db.PDM_Input_Voltage_0x505.visit([&](const auto& value, const auto type_tag)
    {
        using SignalInfo = decltype(type_tag);

        auto match = expected_results.find(std::string(SignalInfo::name));
        if (match != expected_results.end())
        {
            if (close_enough(value, match->second) == false)
            {
                SPDLOG_ERROR("{} : {} != {}", SignalInfo::name, value, match->second);
            }
            else
            {
                SPDLOG_INFO("{} : {} == {}", SignalInfo::name, value, match->second);
            }
        }
    });

    // Test the encode methods.
    std::array<uint8_t, 8> encoded_data = db.PDM_Input_Voltage_0x505.encode();
    SPDLOG_INFO("encoded_data = {:#02x}", fmt::join(encoded_data, ", "));

    if (encoded_data != data)
    {
        SPDLOG_ERROR("encoded_data != test data");
    }
    else
    {
        SPDLOG_INFO("encoded_data == test data");
    }

    return 0;
}