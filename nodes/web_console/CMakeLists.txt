cmake_minimum_required(VERSION 3.10)

project(web_console_node)

# The board's web console: system information, node health, service calls and
# RAUC reflash, served over HTTP.
#
# It is NOT a gateway. The browser is a real zenoh client -- it talks to zenohd
# over ws:// with a wasm module built from this tree's own schemas (see wasm/) --
# so nothing here translates bus traffic. What is served is the half that was
# never on the bus. The zenoh link below is for NodeIdentity and HealthReporter
# only, so the console appears in the health view it serves.
#
# No Qt, deliberately and enforced: the image's recipe asserts that nothing
# outside the gui component links libQt6Core.
add_executable(web_console
    main.cpp
    node_config.cpp
    http_server.cpp
    routes_system.cpp
    routes_update.cpp
    update_routes.cpp
)

target_link_libraries(web_console PRIVATE
    core
    spdlog::spdlog
    cxxopts::cxxopts
    yaml-cpp::yaml-cpp
    nlohmann_json::nlohmann_json
    httplib::httplib
    system_info
    rauc_client
    node_health
    zenoh_pub_sub
    schemas
    capnp
)

# The config parser, including the rule that an unknown key is an error rather
# than a setting that quietly does nothing.
add_executable(web_console_test_node_config
    test_node_config.cpp
    node_config.cpp
)

target_include_directories(web_console_test_node_config PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(web_console_test_node_config PRIVATE
    core
    spdlog::spdlog
    yaml-cpp::yaml-cpp
)

add_project_test(TARGET web_console_test_node_config LABELS web_console unit)

# SHIPPED. See cmake/ProjectInstall.cmake for what that means and what it excludes.
redline_install(TARGET web_console COMPONENT nodes)

# The page this node serves. Declared here rather than in web/ because the
# thing that ships it is the thing that serves it; resource("web") finds it
# beside the executable once installed.
redline_install_data(DIRECTORY ${CMAKE_SOURCE_DIR}/web DESTINATION web COMPONENT nodes)
