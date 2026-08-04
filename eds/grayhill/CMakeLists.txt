# Generate code from EDS
generate_eds_code(canopen_grayhill DS401_3K_C.eds)

# What came out of the generator, checked against the Grayhill manual rather
# than against the EDS -- so a change to the EDS that contradicts the hardware
# fails here instead of on a bench.
add_executable(canopen_test_generated_grayhill
    test_generated_grayhill.cpp
)

target_link_libraries(canopen_test_generated_grayhill
    PRIVATE
        canopen_grayhill
        spdlog::spdlog
)

add_project_test(TARGET canopen_test_generated_grayhill LABELS canopen unit)
