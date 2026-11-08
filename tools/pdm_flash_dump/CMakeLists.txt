# Reads a MoTeC PDM32's flash and RAM over any can:: channel, read-only by
# construction: an adapter, a remote bus over zenoh:, or sims/pdm32. Also turns
# a dumped current calibration into the header sims/pdm32 compiles in.
#
# What it reads from a unit is MoTeC's and the owner's, and stays out of the
# tree (dumps/ and *.s19 are ignored).
add_library(pdm_flash_dump_lib STATIC
    src/session.cpp
    src/dump.cpp
    src/cal_header.cpp
)
target_include_directories(pdm_flash_dump_lib PUBLIC include)
target_link_libraries(pdm_flash_dump_lib
    PUBLIC
        can
        helpers
        nlohmann_json::nlohmann_json
    PRIVATE
        # The unit's CRC, block layout and calibration format.
        pdm32_config
        spdlog::spdlog
)

add_executable(pdm_flash_dump main.cpp)
target_link_libraries(pdm_flash_dump PRIVATE pdm_flash_dump_lib can_backends cli spdlog::spdlog)

# ---------------------------------------------------------------------- tests

# The session against a fake unit, including one that misbehaves, and the
# tool's own refusals; the block checks and the S-record writer.
add_executable(pdm_flash_dump_test_session tests/test_session.cpp)
target_link_libraries(pdm_flash_dump_test_session PRIVATE pdm_flash_dump_lib pdm32_config mctest)
add_project_test(TARGET pdm_flash_dump_test_session LABELS pdm_flash_dump unit)

# cal-header regenerates the committed sims/pdm32 calibration table exactly.
add_executable(pdm_flash_dump_test_cal_header tests/test_cal_header.cpp)
target_link_libraries(pdm_flash_dump_test_cal_header PRIVATE pdm_flash_dump_lib pdm32_config mctest)
target_compile_definitions(pdm_flash_dump_test_cal_header PRIVATE
    CURRENT_CAL_HEADER="${CMAKE_SOURCE_DIR}/sims/pdm32/config/current_cal_dumped.h")
add_project_test(TARGET pdm_flash_dump_test_cal_header LABELS pdm_flash_dump unit)

# A whole dump against pdm32_sim serve over zenoh:. Skips without the firmware.
add_executable(pdm_flash_dump_test_sim tests/test_sim.cpp)
target_link_libraries(pdm_flash_dump_test_sim PRIVATE pdm_flash_dump_lib pdm32_sim_server can_zenoh spdlog::spdlog)
target_compile_definitions(pdm_flash_dump_test_sim PRIVATE PDM32_SCENARIO_DIR="${CMAKE_SOURCE_DIR}/sims/pdm32/scenarios")
add_project_test(TARGET pdm_flash_dump_test_sim LABELS pdm_flash_dump net slow
    ENVIRONMENT "PDM_FIRMWARE_HEX=${PDM_FIRMWARE_HEX}")
