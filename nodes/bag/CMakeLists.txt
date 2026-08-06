cmake_minimum_required(VERSION 3.10)

project(bag_tool)

# A thin CLI over libs/bag. Everything that could be tested without a bus lives
# in the library; what is left here is argument handling, the subscribe/publish
# loops, and presentation.
add_executable(bag_tool
    main.cpp
    record.cpp
    info.cpp
    play.cpp
    reindex.cpp
)

# The binary is `bag`; the target cannot be, because libs/bag already claims
# that name.
set_target_properties(bag_tool PROPERTIES OUTPUT_NAME bag)

target_include_directories(bag_tool PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(bag_tool PRIVATE
    cli
    bag
    zenoh_pub_sub
    schemas
    capnp
    spdlog::spdlog
    nlohmann_json::nlohmann_json
)
