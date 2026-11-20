# What every simulator's `serve` is made of, whatever the device: the serve loop
# (real time or stepped, agent calls on the simulation's thread), each CAN port
# as a can::Channel bridged onto zenoh as can_bridge would bridge a real one, the
# serve config, and the agent-method plumbing (control.h). A device plugs in as a
# ServedDevice (served_device.h).
add_library(sim_host STATIC
    control.cpp
    serve_config.cpp
    server.cpp
    sim_channel.cpp
    debug/debugger.cpp
    debug/methods.cpp
    debug/names.cpp
    debug/session.cpp
    debug/symbols.cpp
)
# Included as "host/server.h".
target_include_directories(sim_host PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(sim_host
    PUBLIC
        can
        can_zenoh
        agent_control_core
        helpers
        nlohmann_json::nlohmann_json
        spdlog::spdlog
    PRIVATE
        cli
        node_config
        yaml-cpp::yaml-cpp
)

# The CAN port as a can::Channel and the serve config, without a device.
add_executable(sim_host_test_channel_config tests/test_sim_channel.cpp)
target_link_libraries(sim_host_test_channel_config PRIVATE sim_host mctest)
add_project_test(TARGET sim_host_test_channel_config LABELS sim_host unit)
