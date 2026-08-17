cmake_minimum_required(VERSION 3.10)

project(bd992_mock)

add_executable(bd992_mock
    main.cpp
    node_config.cpp
    path.cpp
    publishers.cpp
    sources.cpp
    vehicle.cpp
)

target_include_directories(bd992_mock PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(bd992_mock PRIVATE
    # For road_graph/geometry.h -- the distance, bearing and 1e-7 fixed-point
    # vocabulary the whole map half of this tree already speaks. Header-only,
    # but linking the target is how its include directory arrives.
    road_graph

    zenoh_pub_sub
    schemas
    capnp
    spdlog::spdlog
    cxxopts::cxxopts
    yaml-cpp::yaml-cpp
)

# The geometry and the vehicle model, with no bus and no map server: both are
# driven against shapes whose answers are known in closed form.
add_executable(bd992_mock_test_path
    tests/test_path.cpp
    path.cpp
)
target_include_directories(bd992_mock_test_path PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(bd992_mock_test_path PRIVATE road_graph spdlog::spdlog)
add_project_test(TARGET bd992_mock_test_path LABELS bd992_mock unit)

add_executable(bd992_mock_test_vehicle
    tests/test_vehicle.cpp
    path.cpp
    vehicle.cpp
)
target_include_directories(bd992_mock_test_vehicle PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(bd992_mock_test_vehicle PRIVATE road_graph spdlog::spdlog)
add_project_test(TARGET bd992_mock_test_vehicle LABELS bd992_mock unit)

# node_config.cpp is recompiled into the test rather than linked from a library,
# so the parser the test drives is the one the node uses.
add_executable(bd992_mock_test_config
    tests/test_config.cpp
    node_config.cpp
)
target_include_directories(bd992_mock_test_config PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(bd992_mock_test_config PRIVATE
    road_graph zenoh_pub_sub spdlog::spdlog yaml-cpp::yaml-cpp)
add_project_test(TARGET bd992_mock_test_config LABELS bd992_mock unit)
