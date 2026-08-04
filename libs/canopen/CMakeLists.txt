cmake_minimum_required(VERSION 3.10)

project(canopen)

add_library(canopen
    src/bus.cpp
    src/eds_ast.cpp
    src/eds_parser.cpp
    src/lss.cpp
    src/nmt.cpp
    src/pdo_mapping.cpp
    src/sdo.cpp
    src/stub_device.cpp
    src/virtual_bus.cpp
)

target_include_directories(canopen PUBLIC
    include
)

# Lexy is header-only library available from third_party.  SYSTEM so its own
# warnings do not pollute our build.
target_include_directories(canopen SYSTEM PRIVATE
    ${lexy_SOURCE_DIR}/include
)

target_link_libraries(canopen
    PRIVATE
        spdlog::spdlog
    # PUBLIC: canopen/bus.h and everything built on it hand out
    # helpers::CanFrame by value, so a consumer needs helpers' headers too.
    PUBLIC
        helpers
)

# The zenoh transport is a separate target on purpose: it pulls in zenoh, capnp
# and the schemas, and nothing that only wants to parse an EDS or drive a stub
# should have to carry that.
add_library(canopen_zenoh
    src/zenoh_bus.cpp
)

target_link_libraries(canopen_zenoh
    PUBLIC
        canopen
    PRIVATE
        zenoh_pub_sub
        schemas
        capnp
        spdlog::spdlog
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


# Macro to generate C++ helpers from an EDS file.
#
# `eds_file` is resolved against the calling directory. It used to be passed
# through as written, which meant the generator -- run from the binary tree --
# opened nothing, parsed an empty string, and emitted its fallback constants
# without complaint. It is absolute here so that failure cannot recur, and the
# generator now treats an unreadable input as fatal.
#
# A macro substitutes its arguments textually, so the absolute form needs a new
# name: assigning to `eds_file` would not change what `${eds_file}` expands to.
macro(generate_eds_code library_name eds_file)
    get_filename_component(eds_path "${eds_file}" ABSOLUTE BASE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
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
        # --strict: the generated code is only worth anything if it was derived
        # from the file, and a parser regression that fell back to constants
        # would otherwise produce a clean build with wrong numbers in it.
        COMMAND $<TARGET_FILE:canopen_code_gen> --name ${library_name} --input ${eds_path} --output ${output_dir} --silent --strict
        DEPENDS canopen_code_gen ${eds_path}
        MAIN_DEPENDENCY ${eds_path}
        VERBATIM
    )

    add_custom_target(${library_name}_generated ALL DEPENDS ${output_hdr} ${output_src} ${output_node_hdr} ${output_node_src})

    add_library(${library_name} STATIC ${output_src} ${output_node_src})
    add_dependencies(${library_name} ${library_name}_generated)
    target_include_directories(${library_name} PUBLIC ${output_dir})
    # PUBLIC: the generated header includes canopen/pdo_bits.h and
    # helpers/can_frame.h, so anything that includes it needs both.
    target_link_libraries(${library_name} PUBLIC canopen helpers)
endmacro()

# Tests
add_executable(canopen_test_eds_parse
    tests/test_eds_parse.cpp
)

target_link_libraries(canopen_test_eds_parse
    PRIVATE
        canopen
        spdlog::spdlog
        lexy_core
)

# The shipped EDS is part of what is under test: a file this repo generates
# code from should parse and validate clean.
target_compile_definitions(canopen_test_eds_parse PRIVATE
    CANOPEN_EDS_DIR="${CMAKE_SOURCE_DIR}/eds"
)

# SDO, NMT and LSS against a keypad built out of the shipped EDS. The whole
# reconfiguration sequence runs here, with no CAN hardware and no wall clock.
add_executable(canopen_test_runtime
    tests/test_canopen_runtime.cpp
)

target_link_libraries(canopen_test_runtime
    PRIVATE
        canopen
        spdlog::spdlog
)

target_compile_definitions(canopen_test_runtime PRIVATE
    CANOPEN_EDS_DIR="${CMAKE_SOURCE_DIR}/eds"
)

add_project_test(TARGET canopen_test_runtime LABELS canopen unit)
add_project_test(TARGET canopen_test_eds_parse LABELS canopen unit)
