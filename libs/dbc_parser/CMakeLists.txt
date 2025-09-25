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
    set(output_dir ${CMAKE_CURRENT_BINARY_DIR}/${library_name})
    set(output_hdr ${output_dir}/${library_name}.h)
    set(output_parser_hdr ${output_dir}/${library_name}_parser.h)
    set(output_parser_src ${output_dir}/${library_name}_parser.cpp)
    set_source_files_properties(${output_hdr} PROPERTIES GENERATED TRUE)
    set_source_files_properties(${output_parser_hdr} PROPERTIES GENERATED TRUE)
    set_source_files_properties(${output_parser_src} PROPERTIES GENERATED TRUE)

    add_custom_command(
        OUTPUT ${output_hdr} ${output_hdr_prefixed} ${output_parser_hdr} ${output_parser_src}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${output_dir}
        COMMAND $<TARGET_FILE:dbc_code_gen> --name ${library_name} --input ${dbc_file} --output ${output_dir} --silent
        DEPENDS dbc_code_gen ${dbc_file}
        MAIN_DEPENDENCY ${dbc_file}
        VERBATIM
    )

    add_custom_target(${library_name}_header ALL DEPENDS ${output_hdr} ${output_parser_hdr} ${output_parser_src})

    add_library(${library_name} STATIC ${output_parser_src})
    add_dependencies(${library_name} ${library_name}_header)
    target_include_directories(${library_name} PUBLIC ${output_dir})
endmacro()

#add_executable(test_dbc_code_gen
#    tests/test_dbc_code_gen.cpp
#)

#target_link_libraries(test_dbc_code_gen
#    PRIVATE
#    spdlog::spdlog
#    motec_pdm_generic_output
#)