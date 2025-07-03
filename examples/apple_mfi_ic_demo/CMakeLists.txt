add_executable(apple_mfi_ic_demo main.cpp)

target_link_libraries(apple_mfi_ic_demo PRIVATE
    apple_mfi_ic
    spdlog::spdlog
) 