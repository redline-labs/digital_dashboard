cmake_minimum_required(VERSION 3.10)

project(dbc_parser)

add_library(dbc_parser
    src/dbc_lexer.cpp
    src/dbc_parser.cpp
)

target_include_directories(dbc_parser
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(dbc_parser
    PRIVATE
        spdlog::spdlog
)

target_compile_features(dbc_parser PUBLIC cxx_std_23)

# Consumers can link with dbc_parser
add_library(dbc::parser ALIAS dbc_parser)

add_executable(dbc_parser_parse
    tests/parse_dbcs_main.cpp
)

target_link_libraries(dbc_parser_parse
    PRIVATE 
    dbc::parser
    spdlog::spdlog
)


# Code generator executable
add_executable(dbc_code_gen
    src/dbc_code_gen.cpp
    src/generate_h.cpp
)

target_link_libraries(dbc_code_gen
    PRIVATE
    dbc::parser
    common_logging
    cxxopts
)

# Create a CMake macro to generate C++ code from a given dbc file.
# We have a dependency on the dbc_code_gen executable.
macro(generate_dbc_code library_name dbc_file)
    set(output_dir ${CMAKE_BINARY_DIR}/dbc_code_gen/${library_name})
    set(output_hdr ${output_dir}/${library_name}.h)

    add_custom_command(
        OUTPUT ${output_hdr}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${output_dir}
        COMMAND dbc_code_gen --name ${library_name} --input ${dbc_file} --output ${output_dir} --silent
        DEPENDS dbc_code_gen ${dbc_file}
        MAIN_DEPENDENCY ${dbc_file}
        VERBATIM
    )

    add_custom_target(
        ${library_name}_header
        DEPENDS ${output_hdr}
    )

    add_library(${library_name} INTERFACE)
    add_dependencies(${library_name} ${library_name}_header)
    target_include_directories(${library_name} INTERFACE ${output_dir})
endmacro()

generate_dbc_code(motec_pdm_generic_output ${CMAKE_SOURCE_DIR}/dbcs/motec/PDM_Generic_Output.dbc)
generate_dbc_code(motec_e888_rev1 ${CMAKE_SOURCE_DIR}/dbcs/motec/Motec_E888_Rev1.dbc)

add_executable(test_dbc_code_gen
    tests/test_dbc_code_gen.cpp
)

target_link_libraries(test_dbc_code_gen
    PRIVATE
    spdlog::spdlog
    motec_pdm_generic_output
    motec_e888_rev1
)