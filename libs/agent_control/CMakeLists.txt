# Agent-facing control endpoint embedded in the dashboard and editor.
#
# NOTE: not named mcp_* on purpose -- libs/mcp2221a is the Microchip USB-to-I2C
# bridge driver and the collision would be genuinely confusing. The user-facing
# CLI flag is still --mcp.
#
# libs/ is added before dashboard/ in the root CMakeLists, so the Qt targets the
# dashboard finds are not visible here yet; find them again in this scope.
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

# AUTOMOC is enabled in dashboard/'s directory scope, not this one.
set(CMAKE_AUTOMOC ON)

add_library(agent_control STATIC
    capture.cpp
    control_socket.cpp
    error.cpp
    gui_thread.cpp
    input.cpp
    inspector.cpp
    locator.cpp
    log_sink.cpp
    methods.cpp
    server.cpp

    # Listed as a source so AUTOMOC sees the Q_OBJECT in it. Headers under
    # include/ are otherwise skipped by the moc scan, which shows up as a link
    # error for the staticMetaObject rather than anything pointing at the cause.
    include/agent_control/server.h
)

target_include_directories(agent_control PUBLIC include)

target_link_libraries(agent_control
    PUBLIC
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        nlohmann_json::nlohmann_json
        # PUBLIC: log_sink.h exposes a spdlog sink in its interface, so anything
        # installing the ring needs spdlog's headers too.
        spdlog::spdlog
)

# ---------------------------------------------------------------------- tests

add_executable(agent_control_test_framing test_framing.cpp)
target_link_libraries(agent_control_test_framing PRIVATE agent_control spdlog::spdlog)
add_project_test(TARGET agent_control_test_framing LABELS agent_control unit)

add_executable(agent_control_test_log_ring test_log_ring.cpp)
target_link_libraries(agent_control_test_log_ring PRIVATE agent_control spdlog::spdlog)
add_project_test(TARGET agent_control_test_log_ring LABELS agent_control unit)

add_executable(agent_control_test_selector test_selector.cpp)
target_link_libraries(agent_control_test_selector PRIVATE agent_control spdlog::spdlog)
add_project_test(TARGET agent_control_test_selector LABELS agent_control gui)
