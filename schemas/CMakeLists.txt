# Cap'n Proto schemas, and the pub/sub registry derived from them.
#
# To add a schema: drop the .capnp file in this directory and build. Nothing
# here needs editing, and neither does libs/pub_sub -- the registry (the
# schema_type_t enum, the name table, the schema_traits specializations) is
# generated from the schemas themselves by tools/schema_registry_plugin.cpp,
# which reads capnp's own parse rather than pattern-matching the schema text.
# See the comment at the top of that file for what the pattern matching got
# wrong and why none of it can happen now.

# CONFIGURE_DEPENDS makes CMake re-run the glob on every build, so a new schema
# is picked up without a manual reconfigure. That is the whole point: adding a
# schema should not require knowing this file exists.
file(GLOB SCHEMA_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.capnp")

if(NOT SCHEMA_FILES)
    message(FATAL_ERROR "No .capnp files found in ${CMAKE_CURRENT_SOURCE_DIR}")
endif()

# The generator. Built for the host and run at build time, exactly like the
# capnpc-c++ that ships with capnproto -- it is the same plugin interface.
add_executable(capnpc_schema_registry tools/schema_registry_plugin.cpp)
target_link_libraries(capnpc_schema_registry PRIVATE capnp kj)

set(GENERATED_SOURCES)
set(GENERATED_HEADERS)
set(STAGED_OUTPUTS)
set(STAGE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
foreach(SCHEMA_FILE ${SCHEMA_FILES})
    get_filename_component(SCHEMA_NAME ${SCHEMA_FILE} NAME_WE)
    list(APPEND GENERATED_HEADERS "${CMAKE_CURRENT_BINARY_DIR}/${SCHEMA_NAME}.capnp.h")
    list(APPEND GENERATED_SOURCES "${CMAKE_CURRENT_BINARY_DIR}/${SCHEMA_NAME}.capnp.c++")
    list(APPEND STAGED_OUTPUTS
        "${STAGE_DIR}/${SCHEMA_NAME}.capnp.h"
        "${STAGE_DIR}/${SCHEMA_NAME}.capnp.c++")
endforeach()

set(REGISTRY_HEADER "${CMAKE_CURRENT_BINARY_DIR}/pub_sub/schema_registry.h")
set(REGISTRY_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/schema_registry.cpp")

# One invocation for every schema, with both plugins hanging off the same parse.
#
# One invocation rather than one per file because it is the only way imports are
# correct: compiling each schema separately means a file that imports another is
# never regenerated when the imported one changes. Nothing imports anything
# today, and this is what keeps that from becoming a silent staleness bug the
# first time something does.
#
# capnp writes into a staging directory and cmake -E copy_if_different promotes
# the results. Without that, touching any schema would rewrite
# pub_sub/schema_registry.h, and every widget in the tree includes it.
add_custom_command(
    OUTPUT ${GENERATED_HEADERS} ${GENERATED_SOURCES} ${REGISTRY_HEADER} ${REGISTRY_SOURCE}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${STAGE_DIR}
    # Both generators are named by path. `-oc++` would make capnp search $PATH
    # for capnpc-c++, which the old build handled by wrapping the whole command
    # in `cmake -E env` with the vendored capnpc-c++'s directory prepended.
    # Naming the target file directly does the same job without rewriting the
    # environment, and reads as what it is.
    COMMAND $<TARGET_FILE:capnp_tool>
            compile
            -o$<TARGET_FILE:capnpc_cpp>:${STAGE_DIR}
            -o$<TARGET_FILE:capnpc_schema_registry>:${STAGE_DIR}
            --src-prefix=${CMAKE_CURRENT_SOURCE_DIR}
            ${SCHEMA_FILES}
    # Destinations are named as files, not as directories: given a single source
    # and a destination that does not exist yet, `cmake -E copy_if_different`
    # treats the destination as the new file name, so copying into a missing
    # pub_sub/ produces a *file* called pub_sub.
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${STAGE_DIR}/schema_registry.cpp
            ${REGISTRY_SOURCE}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${STAGE_DIR}/pub_sub/schema_registry.h
            ${REGISTRY_HEADER}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${STAGED_OUTPUTS}
            ${CMAKE_CURRENT_BINARY_DIR}
    # capnp_tool and capnpc_cpp are listed because the generated code is tied to
    # the compiler that produced it. Without them, bumping capnproto's GIT_TAG
    # leaves every .capnp.h stale against the new runtime.
    DEPENDS ${SCHEMA_FILES} capnp_tool capnpc_cpp capnpc_schema_registry
    COMMENT "Generating C++ and the schema registry from ${CMAKE_CURRENT_SOURCE_DIR}/*.capnp"
    VERBATIM
)

add_library(schemas STATIC ${GENERATED_SOURCES} ${REGISTRY_SOURCE})

target_include_directories(schemas PUBLIC
    ${CMAKE_CURRENT_BINARY_DIR}
)

# Both PUBLIC: the generated headers include <capnp/schema.h>, and
# pub_sub/schema_registry.h includes reflection/reflection.h. These were PRIVATE
# and only worked because every consumer happened to link capnp itself.
target_link_libraries(schemas PUBLIC
    capnp
    kj
    reflection
)

# The generator, over schemas written the ways the old regex got wrong.
add_executable(schemas_test_registry_plugin
    test_schema_registry_plugin.cpp
)

target_link_libraries(schemas_test_registry_plugin PRIVATE
    spdlog::spdlog
)

target_compile_definitions(schemas_test_registry_plugin PRIVATE
    CAPNP_TOOL="$<TARGET_FILE:capnp_tool>"
    REGISTRY_PLUGIN="$<TARGET_FILE:capnpc_schema_registry>"
    FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test_fixtures"
)

add_dependencies(schemas_test_registry_plugin capnp_tool capnpc_schema_registry)
add_project_test(TARGET schemas_test_registry_plugin LABELS schemas unit)
