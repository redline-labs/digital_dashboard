# Agent-facing control endpoint embedded in the dashboard and editor.
#
# NOTE: not named mcp_* on purpose -- libs/mcp2221a is the Microchip USB-to-I2C
# bridge driver and the collision would be genuinely confusing. The user-facing
# CLI flag is still --mcp.
#
# AUTOMOC is enabled in dashboard/'s directory scope, not this one.
set(CMAKE_AUTOMOC ON)

# The part without Qt: the socket, the JSON-RPC envelope and method table, the
# error codes, typed parameters, the log ring behind app.logs and the --mcp
# option. A headless process (sims/) links only this.
add_library(agent_control_core STATIC
    control_socket.cpp
    dispatcher.cpp
    error.cpp
    log_ring.cpp
    mcp_option.cpp
)
target_include_directories(agent_control_core PUBLIC include)
target_link_libraries(agent_control_core
    PUBLIC
        nlohmann_json::nlohmann_json
        # PUBLIC: log_ring.h exposes a spdlog sink in its interface.
        spdlog::spdlog
        # PUBLIC: mcp_option.h takes cxxopts types, so every program declares
        # --mcp through the same function.
        cxxopts::cxxopts
)

add_library(agent_control STATIC
    app_bootstrap.cpp
    capture.cpp
    gui_thread.cpp
    input.cpp
    inspector.cpp
    locator.cpp
    log_sink.cpp
    methods.cpp
    server.cpp
    zenoh_methods.cpp

    # Listed as a source so AUTOMOC sees the Q_OBJECT in it. Headers under
    # include/ are otherwise skipped by the moc scan, which shows up as a link
    # error for the staticMetaObject rather than anything pointing at the cause.
    include/agent_control/server.h
)

target_include_directories(agent_control PUBLIC include)

target_link_libraries(agent_control
    PUBLIC
        agent_control_core
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
    PRIVATE
        # zenoh.* methods. Both apps already link this; it is here so the
        # capnp<->JSON and topic-observation code is shared with nodes/inspect
        # rather than reimplemented.
        zenoh_pub_sub
)

# ---------------------------------------------------------------------- tests

# The shared --mcp option: default path, an explicit path with '=', and the
# leftover-argument refusal that catches `--mcp /tmp/a.sock`.
add_executable(agent_control_test_bootstrap test_bootstrap.cpp)
target_link_libraries(agent_control_test_bootstrap PRIVATE agent_control spdlog::spdlog)
add_project_test(TARGET agent_control_test_bootstrap LABELS agent_control unit)

# The envelope without Qt or a socket: what a headless process gets.
add_executable(agent_control_test_dispatcher test_dispatcher.cpp)
target_link_libraries(agent_control_test_dispatcher PRIVATE agent_control_core)
add_project_test(TARGET agent_control_test_dispatcher LABELS agent_control unit)

add_executable(agent_control_test_framing test_framing.cpp)
target_link_libraries(agent_control_test_framing PRIVATE agent_control spdlog::spdlog)
add_project_test(TARGET agent_control_test_framing LABELS agent_control unit)

add_executable(agent_control_test_log_ring test_log_ring.cpp)
target_link_libraries(agent_control_test_log_ring PRIVATE agent_control)
add_project_test(TARGET agent_control_test_log_ring LABELS agent_control unit)

# Typed parameter reading: a wrong type is an error naming the parameter, never
# an absent one, and integers are range-checked for their target type.
add_executable(agent_control_test_params test_params.cpp)
target_link_libraries(agent_control_test_params PRIVATE agent_control_core)
add_project_test(TARGET agent_control_test_params LABELS agent_control unit)

# A modal opened by a call: answered as soon as it opens, driven through its own
# event loop, closed by a click. From a second thread, as the socket calls.
add_executable(agent_control_test_modal test_modal.cpp)
target_link_libraries(agent_control_test_modal PRIVATE agent_control spdlog::spdlog)
add_project_test(TARGET agent_control_test_modal LABELS agent_control gui)

add_executable(agent_control_test_selector test_selector.cpp)
target_link_libraries(agent_control_test_selector PRIVATE agent_control spdlog::spdlog)
add_project_test(TARGET agent_control_test_selector LABELS agent_control gui)
