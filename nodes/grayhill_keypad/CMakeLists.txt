cmake_minimum_required(VERSION 3.10)

project(grayhill_keypad)

# The runtime node: PDO only, long-running, never touches the object dictionary.
add_executable(grayhill_keypad
    main.cpp
    node_config.cpp
)

target_link_libraries(grayhill_keypad
    PRIVATE
        canopen_grayhill
        canopen
        canopen_zenoh

        zenoh_pub_sub
        spdlog::spdlog
        cxxopts::cxxopts
        yaml-cpp::yaml-cpp
        capnp
        schemas
        helpers
)

target_compile_definitions(grayhill_keypad PRIVATE
    GRAYHILL_DEFAULT_CONFIG="${CMAKE_SOURCE_DIR}/configs/grayhill_keypad/grayhill_keypad.yaml"
)

# The reconfiguration tool: SDO/LSS/NMT only, one-shot, and writing non-volatile
# memory is its whole job. Deliberately a separate binary from the runtime node
# -- neither has to reason about the other's state because neither does the
# other's work.
add_executable(grayhill_keypad_reconfigure
    reconfigure_main.cpp
    reconfigure_config.cpp
    reconfigure_plan.cpp
)

target_link_libraries(grayhill_keypad_reconfigure
    PRIVATE
        canopen
        canopen_zenoh
        spdlog::spdlog
        cxxopts::cxxopts
        yaml-cpp::yaml-cpp
        helpers
)

target_compile_definitions(grayhill_keypad_reconfigure PRIVATE
    GRAYHILL_EDS_PATH="${CMAKE_SOURCE_DIR}/eds/grayhill/DS401_3K_C.eds"
)

# Config parsing and plan building, which is where the tool's judgment lives:
# what an absent key means, which values the EDS forbids, and the ordering that
# keeps a half-applied configuration recoverable.
add_executable(grayhill_test_reconfigure
    test_reconfigure.cpp
    reconfigure_config.cpp
    reconfigure_plan.cpp
)

target_link_libraries(grayhill_test_reconfigure
    PRIVATE
        canopen
        spdlog::spdlog
        yaml-cpp::yaml-cpp
        helpers
)

target_compile_definitions(grayhill_test_reconfigure PRIVATE
    GRAYHILL_EDS_PATH="${CMAKE_SOURCE_DIR}/eds/grayhill/DS401_3K_C.eds"
)

add_project_test(TARGET grayhill_test_reconfigure LABELS grayhill canopen unit)
