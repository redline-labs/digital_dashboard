add_library(mcp2221a mcp2221a.cpp)

target_include_directories(mcp2221a PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(mcp2221a PUBLIC
    hidapi
    spdlog::spdlog
) 