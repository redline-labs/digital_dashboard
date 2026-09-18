cmake_minimum_required(VERSION 3.10)

project(web_console_node)

# The board's web console: system information, node health, service calls and
# RAUC reflash, served over HTTP.
#
# It holds a zenoh session and serves health and services as JSON: the browser
# is meant to read the bus itself through wasm/, but that transport is not
# proven yet (see routes_health.cpp), so for now the node is the bus client.

# No Qt, deliberately and enforced: the image's recipe asserts that nothing
# outside the gui component links libQt6Core.

# Everything but main(), so the tests can serve the real routes without the
# node's bus session or RAUC behind them.
add_library(web_console_routes STATIC
    node_config.cpp
    http_server.cpp
    routes_system.cpp
    routes_health.cpp
    routes_services.cpp
    service_routes.cpp
    routes_update.cpp
    update_routes.cpp
)

target_include_directories(web_console_routes PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(web_console_routes PUBLIC
    core
    spdlog::spdlog
    yaml-cpp::yaml-cpp
    nlohmann_json::nlohmann_json
    httplib::httplib
    system_info
    rauc_client
    node_health
    cli
    zenoh_pub_sub
    schemas
    capnp
)

add_executable(web_console main.cpp)

target_link_libraries(web_console PRIVATE
    web_console_routes
    cxxopts::cxxopts
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

# What the routes make of a client's input, without a socket.
add_executable(web_console_test_requests test_requests.cpp)
target_link_libraries(web_console_test_requests PRIVATE web_console_routes)
add_project_test(TARGET web_console_test_requests LABELS web_console unit)

# The routes over a real loopback socket: uploads, the progress stream and
# shutdown. No bus and no RAUC -- the routes are built without either.
add_executable(web_console_test_http test_http.cpp)
target_link_libraries(web_console_test_http PRIVATE web_console_routes)
add_project_test(TARGET web_console_test_http LABELS web_console net)

# SHIPPED. See cmake/ProjectInstall.cmake for what that means and what it excludes.
redline_install(TARGET web_console COMPONENT nodes)

# The page this node serves. Declared here rather than in web/ because the
# thing that ships it is the thing that serves it; resource("web") finds it
# beside the executable once installed.
redline_install_data(DIRECTORY ${CMAKE_SOURCE_DIR}/web DESTINATION web COMPONENT nodes)
