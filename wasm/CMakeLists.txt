# The browser half of the web console: this project's capnp decoding, compiled
# to WebAssembly.
#
# WHY THIS IS A SEPARATE ENTRY POINT AND NOT A TARGET IN THE MAIN BUILD.
# `emcmake cmake -S .` at the repo root cannot work, for two independent
# reasons: the root CMakeLists.txt does find_package(Qt6 REQUIRED ...), and
# add_subdirectory(third_party) fetches zenoh-c, whose cargo build produces a
# NATIVE static library from Rust. Neither has a wasm answer. So this is its own
# project that reaches back into the tree for exactly the pieces that are free of
# both -- verified, not assumed:
#
#   schema_layout_hash.cpp -> pub_sub/schema_layout.h -> <capnp/schema.h> + std
#   schema_registry.h      -> <capnp/schema.h> + reflection/reflection.h (std only)
#   node_health codec.h/classify.h say "No zenoh here" in their own comments
#
# The point of compiling OUR code rather than reimplementing it in TypeScript is
# the 8-byte layout fingerprint: it is FNV-1a over node ids, field slot offsets
# and discriminants, and a browser that computes it differently does not fail
# loudly -- it silently drops every sample. One implementation, two targets.
cmake_minimum_required(VERSION 3.16)
project(redline_wasm LANGUAGES C CXX)

if(NOT EMSCRIPTEN)
    message(FATAL_ERROR
        "This project only builds under Emscripten. Use tools/build-wasm.sh, "
        "or configure with `emcmake cmake -S wasm -B build-wasm`.")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release")
endif()

get_filename_component(REDLINE_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

# schemas/ declares three test executables. add_project_test() already drops them
# from `all` when BUILD_TESTING is off (it sets EXCLUDE_FROM_ALL and returns), but
# their target_link_libraries() still has to RESOLVE at generate time -- a missing
# link target is a hard error whether or not the target is built. That is the only
# reason spdlog is fetched below.
set(BUILD_TESTING OFF)

include(FetchContent)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)
set(FETCHCONTENT_QUIET OFF)

include(${REDLINE_ROOT}/third_party/capnproto.cmake)
include(${REDLINE_ROOT}/third_party/spdlog.cmake)
# capnp_json.cpp needs it. Verified zenoh-free: capnp_json.{h,cpp} reach only
# capnp, pub_sub/capnp_payload.h and helpers/hex.h, all of which are header-only
# or capnp+std. That is what lets the browser run the SAME capnpToJson the Qt
# app and `inspect` use, instead of a second implementation that can disagree.
include(${REDLINE_ROOT}/third_party/nlohmann_json.cmake)

# CAPNPROTO'S -pthread, WHICH EMSCRIPTEN ACCEPTS AND SHOULD NOT.
#
# capnproto's c++/CMakeLists.txt does add_compile_options(-pthread) for non-Android
# UNIX, and src/kj/CMakeLists.txt links `pthread`, both under `if(UNIX ...)`.
# Emscripten DEFINES UNIX, so a wasm build inherits both -- and -pthread under
# emcc means SharedArrayBuffer, which means the page must be served with COOP and
# COEP headers. For a decode-only module that is a large cost for nothing.
#
# UNVERIFIED as of writing: strip it back off here and check the link line in the
# spike (emcc -v). If this proves insufficient, the fallback is a PATCH_COMMAND on
# the capnproto fetch, which is uglier but deterministic.
foreach(_capnp_target kj capnp)
    if(TARGET ${_capnp_target})
        get_target_property(_opts ${_capnp_target} INTERFACE_COMPILE_OPTIONS)
        if(_opts)
            list(REMOVE_ITEM _opts "-pthread")
            set_target_properties(${_capnp_target} PROPERTIES INTERFACE_COMPILE_OPTIONS "${_opts}")
        endif()
        get_target_property(_libs ${_capnp_target} INTERFACE_LINK_LIBRARIES)
        if(_libs)
            list(REMOVE_ITEM _libs "pthread" "-pthread" "Threads::Threads")
            set_target_properties(${_capnp_target} PROPERTIES INTERFACE_LINK_LIBRARIES "${_libs}")
        endif()
    endif()
endforeach()

# reflection is header-only and INTERFACE. Declared here rather than reached by
# add_subdirectory(libs/reflection), which would also declare test_reflection and
# call redline_install() -- neither of which means anything in a browser.
add_library(reflection INTERFACE)
target_include_directories(reflection INTERFACE ${REDLINE_ROOT}/libs/reflection/include)

# The codegen knob. capnp, capnpc-c++ and capnpc_schema_registry have to RUN on
# the build host, and here they would be wasm. tools/build-wasm.sh stages all
# three into one directory and passes it; NativeCodegen.cmake resolves them there.
list(APPEND CMAKE_MODULE_PATH ${REDLINE_ROOT}/cmake)
include(ProjectTest)
include(NativeCodegen)

add_subdirectory(${REDLINE_ROOT}/schemas schemas)

# Those three tools are ordinary add_executable()s in the tree, so they are in
# `all` and would be compiled to wasm for nothing. The staged host copies are
# what actually runs.
foreach(_host_tool capnp_tool capnpc_cpp capnpc_schema_registry)
    if(TARGET ${_host_tool})
        set_target_properties(${_host_tool} PROPERTIES EXCLUDE_FROM_ALL TRUE)
    endif()
endforeach()

option(REDLINE_WASM_WITH_PICO "Include the zenoh-pico client in the module" ON)
if(REDLINE_WASM_WITH_PICO)
    include(${REDLINE_ROOT}/third_party/zenoh-pico.cmake)
endif()

# The module. Sources are named individually rather than by linking the tree's
# libraries: `zenoh_pub_sub` and `node_health` both link zenoh, and the whole
# point is to take the parts that do not.
add_executable(redline_wasm
    bindings.cpp
    ${REDLINE_ROOT}/libs/pub_sub/capnp_json.cpp
    ${REDLINE_ROOT}/libs/pub_sub/schema_layout.cpp
    ${REDLINE_ROOT}/libs/pub_sub/schema_layout_hash.cpp
    ${REDLINE_ROOT}/libs/node_health/src/state.cpp
    ${REDLINE_ROOT}/libs/node_health/src/codec.cpp
    ${REDLINE_ROOT}/libs/node_health/src/classify.cpp
)

target_include_directories(redline_wasm PRIVATE
    ${REDLINE_ROOT}/libs/pub_sub/include
    ${REDLINE_ROOT}/libs/node_health/include
    ${REDLINE_ROOT}/libs/helpers/include
)

target_link_libraries(redline_wasm PRIVATE
    schemas capnp kj spdlog::spdlog nlohmann_json::nlohmann_json)
if(REDLINE_WASM_WITH_PICO)
    target_link_libraries(redline_wasm PRIVATE zenohpico::lib)
    target_compile_definitions(redline_wasm PRIVATE REDLINE_WASM_WITH_PICO=1)
endif()

# -fexceptions, not -fno-exceptions: KJ throws kj::Exception and capnp's
# SchemaLoader depends on it. This is why wasi-sdk was not an option.
target_compile_options(redline_wasm PRIVATE -fexceptions -Os)
# ASYNCIFY, because of one line upstream.
#
# _z_ws_emscripten_open() calls connect(), breaks immediately -- its own comment
# says connect returns -1 even when the socket is actually opening -- and then
# does a single z_sleep_ms(100) as a stated workaround "as connect returns before
# the websocket is actually open". On this platform z_sleep_ms is
# emscripten_sleep, which aborts outright without asyncify support:
#
#   Aborted(Please compile your program with async support ...)
#
# So the transport is not inherently blocking; it has one sleep papering over an
# async handshake. ASYNCIFY_ONLY scopes the instrumentation to that call path
# rather than paying for the whole module -- capnp decoding, SchemaLoader and the
# layout hashing have no business being rewound.
#
# Note what this does NOT do: ASYNCIFY_EXPORTS is deprecated and empty, so embind
# exports stay synchronous. decodeToJson(), healthFromPayload() and the layout
# hash functions keep working exactly as they already do, and app.js is unaffected.
target_link_options(redline_wasm PRIVATE
    -sASYNCIFY
    -fexceptions
    -lembind
    -sMODULARIZE=1
    -sEXPORT_NAME=createRedlineModule
    -sALLOW_MEMORY_GROWTH=1
    -sENVIRONMENT=web,node
    -sEXPORTED_RUNTIME_METHODS=['HEAPU8']
    -Os
)
set_target_properties(redline_wasm PROPERTIES OUTPUT_NAME "redline")
