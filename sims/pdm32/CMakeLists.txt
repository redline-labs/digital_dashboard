# The MoTeC PDM32, emulated: its application firmware on a GZ60 (libs/gz60),
# the board and what is wired to its connector, and the whole-system assembly
# that runs scenarios and answers the agent interface.
#
# The firmware is MoTeC's and is not in the tree. Point PDM_FIRMWARE_HEX at
# PDM Manager's pdm.hex (the cache variable here, or the environment at run
# time). Without it the simulator cannot start, and the tests that need it skip.
set(PDM_FIRMWARE_HEX "$ENV{HOME}/src/motec/MoTeC/PDM Manager/1.9/pdm.hex"
    CACHE FILEPATH "MoTeC PDM32 application image (S-records), from PDM Manager 1.9")

# Layers, each including only those below it (tools/check_layers.py):
#   board/   the PDM32 board: output switches, input conditioning, sense
#            multiplexers, supply, the boot ROM stand-in
#   plant/   the loads and sources outside the connector
#   config/  the flash images the bootloader would hold: configuration,
#            current calibration, factory data
#   top/     the whole system: instance, session, scenarios, stimulus,
#            debugger, the agent methods
# config/ on its own: the unit's formats (CRC, layout, calibration), which
# tools/pdm_flash_dump uses without the emulator.
add_library(pdm32_config STATIC
    config/config_builder.cpp
    config/crc16.cpp
    config/current_cal.cpp
    config/image_set.cpp
)
target_include_directories(pdm32_config PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(pdm32_config PUBLIC hc08 nlohmann_json::nlohmann_json)

add_library(pdm32_sim_core STATIC
    board/boot_hle.cpp
    board/hi_side_switch.cpp
    board/pdm32.cpp
    board/wiring.cpp
    plant/loads.cpp
    top/control.cpp
    top/debugger.cpp
    top/instance.cpp
    top/methods.cpp
    top/probe.cpp
    top/scenario.cpp
    top/sim_session.cpp
    top/stimulus.cpp
    top/sim_channel.cpp
    top/symbols.cpp
)
# Included as "pdm32/board/pdm32.h".
target_include_directories(pdm32_sim_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(pdm32_sim_core
    PUBLIC
        pdm32_config
        can
        gz60
        hc08
        emu
        agent_control_core
        dbc_parser
        nlohmann_json::nlohmann_json
        spdlog::spdlog
    PRIVATE
        core
)

# `serve`: the PDM on the bus. A library so the tests can run it in-process.
add_library(pdm32_sim_server STATIC
    server.cpp
    serve_config.cpp
)
target_include_directories(pdm32_sim_server PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pdm32_sim_server
    PUBLIC
        pdm32_sim_core
        can_zenoh
        agent_control_core
    PRIVATE
        cli
        node_config
        yaml-cpp::yaml-cpp
)

add_executable(pdm32_sim
    main.cpp
    verbs.cpp
)
target_link_libraries(pdm32_sim PRIVATE pdm32_sim_server pdm32_sim_core core cli node_health zenoh_pub_sub)

# ---------------------------------------------------------------------- tests

set(_pdm32_firmware_env "PDM_FIRMWARE_HEX=${PDM_FIRMWARE_HEX}")

# The board and the synthetic flash images, without firmware.
add_executable(pdm32_test_board
    tests/test_board.cpp
    tests/test_config.cpp
)
target_link_libraries(pdm32_test_board PRIVATE pdm32_sim_core mctest)
# flat_env.h: the flat memory map the HC08 tests run programs in.
target_include_directories(pdm32_test_board PRIVATE ${CMAKE_SOURCE_DIR}/libs/hc08/tests)
add_project_test(TARGET pdm32_test_board LABELS pdm32 unit)

# The firmware running on the whole system: the delivery scenario, the agent
# methods. Skips without the firmware image.
add_executable(pdm32_test_system
    tests/test_system.cpp
    tests/test_rpc.cpp
)
target_link_libraries(pdm32_test_system PRIVATE pdm32_sim_core mctest)
target_compile_definitions(pdm32_test_system PRIVATE PDM32_SCENARIO_DIR="${CMAKE_CURRENT_SOURCE_DIR}/scenarios")
add_project_test(TARGET pdm32_test_system LABELS pdm32 slow ENVIRONMENT ${_pdm32_firmware_env})

# The device's CAN port as a can::Channel, and the serve config, without firmware.
add_executable(pdm32_test_sim_channel
    tests/test_sim_channel.cpp
)
target_link_libraries(pdm32_test_sim_channel PRIVATE pdm32_sim_server mctest)
add_project_test(TARGET pdm32_test_sim_channel LABELS pdm32 unit)

# serve in-process on a real zenoh session: the data stream at its rate on the
# rx topic, a tx frame into the PDM, the methods answering from the running
# simulation. Skips without the firmware image.
add_executable(pdm32_test_serve tests/test_serve.cpp)
target_link_libraries(pdm32_test_serve PRIVATE pdm32_sim_server zenoh_pub_sub schemas capnp spdlog::spdlog)
target_include_directories(pdm32_test_serve PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_compile_definitions(pdm32_test_serve PRIVATE PDM32_SCENARIO_DIR="${CMAKE_CURRENT_SOURCE_DIR}/scenarios")
add_project_test(TARGET pdm32_test_serve LABELS pdm32 net slow ENVIRONMENT ${_pdm32_firmware_env})

# Include direction between the layers above.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(NAME pdm32_test_layers COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_layers.py)
    set_tests_properties(pdm32_test_layers PROPERTIES LABELS "pdm32;unit")
endif()

# The sim_* MCP tools against this build's pdm32_sim, through the supervisor
# (tools/mcp_dashboard/tests). Needs uv; skips without the firmware image.
find_program(UV_PROGRAM uv)
if(UV_PROGRAM)
    add_test(NAME pdm32_test_mcp
        COMMAND ${UV_PROGRAM} run --with pytest pytest -q tests
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/tools/mcp_dashboard)
    set_tests_properties(pdm32_test_mcp PROPERTIES LABELS "pdm32;slow"
        ENVIRONMENT "REDLINE_BUILD_DIR=${CMAKE_BINARY_DIR};${_pdm32_firmware_env};PUB_SUB_NO_DISCOVERY=1")
endif()

# The engines on the real firmware. A measurement, not a test.
add_executable(pdm32_bench bench/bench_core.cpp)
target_link_libraries(pdm32_bench PRIVATE pdm32_sim_core core)
target_compile_definitions(pdm32_bench PRIVATE PDM32_SCENARIO_DIR="${CMAKE_CURRENT_SOURCE_DIR}/scenarios")
set_target_properties(pdm32_bench PROPERTIES EXCLUDE_FROM_ALL TRUE)
