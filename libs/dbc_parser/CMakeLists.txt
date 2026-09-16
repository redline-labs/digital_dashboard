cmake_minimum_required(VERSION 3.10)

project(dbc_parser)

# generate_dbc_code() runs dbc_code_gen during the build, which a cross build
# cannot do with the copy it just built for the target.
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/NativeCodegen.cmake)

add_library(dbc_parser
    src/ast.cpp
    src/dbc_lexer.cpp
    src/dbc_parser.cpp
)

target_include_directories(dbc_parser
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# No logging dependency on purpose: the parser returns its diagnostics rather
# than printing them, so a caller decides what to do about a bad file.
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
# Generation itself is a pure function of the parsed database, so it can be
# driven from a DBC held in a string literal. dbc_code_gen is only the CLI on
# top of it, and dbc_parser_test_codegen calls the library directly.
add_library(dbc_codegen STATIC
    src/generate_sources.cpp
    src/generate_cpp_header.cpp
    src/generate_cpp_parser_header.cpp
    src/generate_cpp_parser_source.cpp
)

target_link_libraries(dbc_codegen
    PUBLIC
    dbc::parser
    PRIVATE
    spdlog::spdlog
)

add_library(dbc::codegen ALIAS dbc_codegen)

add_executable(dbc_code_gen
    src/dbc_code_gen.cpp
)

target_link_libraries(dbc_code_gen
    PRIVATE
    dbc::codegen
    spdlog::spdlog
    cxxopts
)

# Generate a C++ library from a DBC file.
#
# A function rather than a macro, so output_dir and friends stay local instead
# of leaking into the caller's scope.
#
# Only the three fixed outputs are declared. The per-message headers are
# written by the same invocation and cannot be named at configure time, but
# they are always rewritten together with these -- and deleting one by hand
# fails loudly at compile time, because the main header includes it.
function(generate_dbc_code library_name dbc_file)
    # Per call, not once at the top of this file: this function is called from
    # sibling directories (dbcs/), where a variable set here is invisible.
    redline_codegen_tool(codegen_command codegen_depends dbc_code_gen dbc_code_gen)

    set(output_dir ${CMAKE_CURRENT_BINARY_DIR}/${library_name})
    set(output_hdr ${output_dir}/${library_name}.h)
    set(output_common_hdr ${output_dir}/${library_name}_common.h)
    set(output_parser_hdr ${output_dir}/${library_name}_parser.h)
    set(output_parser_src ${output_dir}/${library_name}_parser.cpp)

    add_custom_command(
        OUTPUT ${output_hdr} ${output_common_hdr} ${output_parser_hdr} ${output_parser_src}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${output_dir}
        COMMAND ${codegen_command} --name ${library_name} --input ${dbc_file} --output ${output_dir} --silent
        DEPENDS ${codegen_depends} ${dbc_file}
        MAIN_DEPENDENCY ${dbc_file}
        COMMENT "Generating ${library_name} from ${dbc_file}"
        VERBATIM
    )

    add_custom_target(${library_name}_header ALL DEPENDS ${output_hdr} ${output_parser_hdr} ${output_parser_src})

    add_library(${library_name} STATIC ${output_parser_src})
    add_dependencies(${library_name} ${library_name}_header)
    target_include_directories(${library_name} PUBLIC ${output_dir})
    target_compile_features(${library_name} PUBLIC cxx_std_23)
endfunction()

# Tests own their own synthetic DBCs, so this has to come after
# generate_dbc_code() is defined above.
#
# Gated because this is a library a consumer may want on its own: everything in
# tests/ calls add_project_test(), which is defined by the root project's
# cmake/ProjectTest.cmake, and a consumer that just wanted a DBC parser had to
# stub that function to get past this line.
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

# SHIPPED. See cmake/ProjectInstall.cmake for what that means and what it excludes.
redline_install(TARGET dbc_code_gen COMPONENT nodes)
redline_install(TARGET dbc_parser_parse COMPONENT nodes)
