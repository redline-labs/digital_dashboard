cmake_minimum_required(VERSION 3.10)

project(canopen)

add_library(canopen
    src/eds_parser.cpp
    src/transport.cpp
)

target_include_directories(canopen PUBLIC
    include
)

# Lexy is header-only library available from third_party
target_include_directories(canopen PRIVATE
    ${lexy_SOURCE_DIR}/include
)

target_link_libraries(canopen
    PRIVATE
        spdlog::spdlog
        helpers
)

# Code generator
add_executable(canopen_code_gen
    tools/canopen_code_gen.cpp
)

target_link_libraries(canopen_code_gen
    PRIVATE
        canopen
        spdlog::spdlog
        cxxopts
)


# Macro to generate C++ helpers from an EDS file
macro(generate_eds_code library_name eds_file)
    set(output_dir ${CMAKE_CURRENT_BINARY_DIR}/${library_name})
    set(output_hdr ${output_dir}/${library_name}_helpers.h)
    set(output_src ${output_dir}/${library_name}_helpers.cpp)
    set(output_node_hdr ${output_dir}/${library_name}_node.h)
    set(output_node_src ${output_dir}/${library_name}_node.cpp)
    set_source_files_properties(${output_hdr} PROPERTIES GENERATED TRUE)
    set_source_files_properties(${output_src} PROPERTIES GENERATED TRUE)
    set_source_files_properties(${output_node_hdr} PROPERTIES GENERATED TRUE)
    set_source_files_properties(${output_node_src} PROPERTIES GENERATED TRUE)

    add_custom_command(
        OUTPUT ${output_hdr} ${output_src} ${output_node_hdr} ${output_node_src}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${output_dir}
        COMMAND $<TARGET_FILE:canopen_code_gen> --name ${library_name} --input ${eds_file} --output ${output_dir} --silent
        DEPENDS canopen_code_gen ${eds_file}
        MAIN_DEPENDENCY ${eds_file}
        VERBATIM
    )

    add_custom_target(${library_name}_generated ALL DEPENDS ${output_hdr} ${output_src} ${output_node_hdr} ${output_node_src})

    add_library(${library_name} STATIC ${output_src} ${output_node_src})
    add_dependencies(${library_name} ${library_name}_generated)
    target_include_directories(${library_name} PUBLIC ${output_dir})
    target_link_libraries(${library_name} PRIVATE helpers)
endmacro()

# Tests
add_executable(eds_parse_test
    tests/eds_parse_test.cpp
)

target_link_libraries(eds_parse_test
    PRIVATE
        canopen
        spdlog::spdlog
        lexy_core
)
