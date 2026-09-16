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
#
# schema_layout_hash.cpp comes along as a source file, not as a link against
# zenoh_pub_sub: that library consumes the registry this plugin generates, so
# depending on it would be a cycle. The file is written to be compiled twice for
# exactly this reason -- the fingerprints baked into the registry and the ones a
# running program computes from a recorded descriptor have to come out of one
# implementation.
add_executable(capnpc_schema_registry
    tools/schema_registry_plugin.cpp
    ../libs/pub_sub/schema_layout_hash.cpp
)
target_include_directories(capnpc_schema_registry PRIVATE ../libs/pub_sub/include)
target_link_libraries(capnpc_schema_registry PRIVATE capnp kj)

# ... unless the host is not the target, in which case none of the three tools
# below is runnable by the build that produced them.
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/NativeCodegen.cmake)
redline_codegen_tool(CAPNP_COMMAND CAPNP_DEPENDS capnp_tool capnp)
redline_codegen_tool(CAPNPC_CPP_COMMAND CAPNPC_CPP_DEPENDS capnpc_cpp capnpc-c++)
redline_codegen_tool(REGISTRY_PLUGIN_COMMAND REGISTRY_PLUGIN_DEPENDS
                     capnpc_schema_registry capnpc_schema_registry)

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
# never regenerated when the imported one changes. Several files do import --
# gsof_*.capnp share gsof_common.capnp, map_*.capnp share map_common.capnp --
# so this is load-bearing rather than precautionary.
#
# capnp writes into a staging directory and cmake -E copy_if_different promotes
# the results. Without that, touching any schema would rewrite
# pub_sub/schema_registry.h, and every widget in the tree includes it.
#
# The rule's only OUTPUT is a stamp file; the generated sources and headers are
# BYPRODUCTS. That split is what makes the build settle, and it is worth
# spelling out, because the obvious spelling -- list the generated files as the
# OUTPUTs -- does not.
#
# copy_if_different deliberately leaves an unchanged file's mtime alone, and a
# build tool decides whether a rule is done by looking at exactly that. With the
# generated files as OUTPUTs, the rule's own outputs came back older than the
# schemas that produced them, so it was never satisfied. Worse, the Makefile
# generator runs each target's build.make twice, once for `depend` and once for
# `build`, as two separate make processes -- so capnp ran twice on every build,
# up to date or not. A stamp whose mtime always moves is the thing make can
# actually see finish.
#
# BYPRODUCTS rather than OUTPUTS for the real files also avoids the Makefile
# generator's other habit: it gives the first OUTPUT the recipe and hangs
# `touch_nocreate` stubs for the rest off it, which would stamp a new mtime on
# all 90 generated files any time the first one changed, undoing the promotion
# above. The generated files still get marked GENERATED and still get cleaned;
# `schemas` just reaches them through add_dependencies below instead of through
# the output graph, and the compiler's own depfiles carry the header edges.
set(CAPNP_STAMP "${CMAKE_CURRENT_BINARY_DIR}/schemas.capnp.stamp")

add_custom_command(
    OUTPUT ${CAPNP_STAMP}
    BYPRODUCTS ${GENERATED_HEADERS} ${GENERATED_SOURCES}
               ${REGISTRY_HEADER} ${REGISTRY_SOURCE}
               ${STAGED_OUTPUTS}
               ${STAGE_DIR}/schema_registry.cpp
               ${STAGE_DIR}/pub_sub/schema_registry.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${STAGE_DIR}
    # Both generators are named by path. `-oc++` would make capnp search $PATH
    # for capnpc-c++, which the old build handled by wrapping the whole command
    # in `cmake -E env` with the vendored capnpc-c++'s directory prepended.
    # Naming the target file directly does the same job without rewriting the
    # environment, and reads as what it is.
    COMMAND ${CAPNP_COMMAND}
            compile
            -o${CAPNPC_CPP_COMMAND}:${STAGE_DIR}
            -o${REGISTRY_PLUGIN_COMMAND}:${STAGE_DIR}
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
    COMMAND ${CMAKE_COMMAND} -E touch ${CAPNP_STAMP}
    # capnp_tool and capnpc_cpp are listed because the generated code is tied to
    # the compiler that produced it. Without them, bumping capnproto's GIT_TAG
    # leaves every .capnp.h stale against the new runtime.
    DEPENDS ${SCHEMA_FILES}
            ${CAPNP_DEPENDS} ${CAPNPC_CPP_DEPENDS} ${REGISTRY_PLUGIN_DEPENDS}
    COMMENT "Generating C++ and the schema registry from ${CMAKE_CURRENT_SOURCE_DIR}/*.capnp"
    VERBATIM
)

add_custom_target(schemas_codegen DEPENDS ${CAPNP_STAMP})

add_library(schemas STATIC ${GENERATED_SOURCES} ${REGISTRY_SOURCE})

# The ordering edge the output graph no longer provides: the .capnp.c++ files
# have to exist before this library compiles them.
add_dependencies(schemas schemas_codegen)

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

# The serialized schema descriptors -- the schema as DATA, which is what a bag
# file embeds so a foreign consumer can decode a recording.
#
# Loaded through capnp::SchemaLoader exactly as such a consumer would, then
# compared against capnp::Schema::from<> in-process. Two independent routes to
# the same answer, because a descriptor that is subtly wrong still LOADS -- it
# just cannot decode the affected field, long after the recording was made.
add_executable(schemas_test_schema_descriptor
    test_schema_descriptor.cpp
)

target_link_libraries(schemas_test_schema_descriptor PRIVATE
    schemas
    capnp
    kj
)

add_project_test(TARGET schemas_test_schema_descriptor LABELS schemas unit)

# The doc comments the generator lifts out of the schemas, looked up the way a
# runtime form builder does: by node id and field index.
add_executable(schemas_test_doc_comments
    test_doc_comments.cpp
)

target_link_libraries(schemas_test_doc_comments PRIVATE
    schemas
    capnp
    kj
)

add_project_test(TARGET schemas_test_doc_comments LABELS schemas unit)
