add_library(apple_mfi_ic apple_mfi_ic.cpp)

target_include_directories(apple_mfi_ic PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(apple_mfi_ic PUBLIC
    mcp2221a
    spdlog::spdlog
) 