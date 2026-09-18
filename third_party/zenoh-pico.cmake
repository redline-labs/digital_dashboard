# zenoh-pico, for the BROWSER only: a tab cannot be a zenoh peer, so the web
# console's wasm module is a client of zenohd's ws/ listener, and pico is the
# only zenoh that compiles to wasm. wasm/CMakeLists.txt is the only consumer;
# nothing on the board or a workstation links pico.
#
# Pinned to match zenoh-c. Do not reach for the copy zenoh-cpp vendors
# (_deps/zenoh-cpp-src/zenoh-pico): it tracks a branch and is 1.9.0.
get_filename_component(REDLINE_PATCH_DIR "${CMAKE_CURRENT_LIST_DIR}/../patches" ABSOLUTE)

set(ZENOH_PICO_GIT_TAG 1.10.0)
set(ZENOH_PICO_PATCHES
    zenoh_pico_emscripten_stddef.patch
    zenoh_pico_emscripten_ws_closed_link.patch
)
list(TRANSFORM ZENOH_PICO_PATCHES PREPEND "${REDLINE_PATCH_DIR}/" OUTPUT_VARIABLE _pico_patch_paths)

FetchContent_Declare(
    zenoh-pico
    GIT_REPOSITORY https://github.com/eclipse-zenoh/zenoh-pico.git
    GIT_TAG ${ZENOH_PICO_GIT_TAG}
    GIT_SHALLOW TRUE
    PATCH_COMMAND git apply ${_pico_patch_paths}
)

# ZP_PLATFORM, not ZP_SYSTEM_LAYER: 1.10.0 renamed it and fails the configure
# if the old name is defined at all. The emscripten profile is what supplies
# the ws link, and pico refuses Z_FEATURE_LINK_WS on any other platform.
set(ZP_PLATFORM "emscripten" CACHE STRING "" FORCE)
set(ZENOH_LOG "debug" CACHE STRING "" FORCE)

# Emscripten cannot build shared libraries, and pico defaults to one.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(Z_FEATURE_LINK_WS 1 CACHE STRING "" FORCE)
set(Z_FEATURE_LINK_TCP 0 CACHE STRING "" FORCE)
set(Z_FEATURE_LINK_UDP_UNICAST 0 CACHE STRING "" FORCE)
set(Z_FEATURE_LINK_UDP_MULTICAST 0 CACHE STRING "" FORCE)
set(Z_FEATURE_SCOUTING 0 CACHE STRING "" FORCE)

# Single-threaded; JS pumps the session (wasm/bus.cpp). Threads under emcc
# mean SharedArrayBuffer, and so COOP/COEP headers on every page.
set(Z_FEATURE_MULTI_THREAD 0 CACHE STRING "" FORCE)
set(Z_FEATURE_MULTICAST_TRANSPORT 0 CACHE STRING "" FORCE)

# Discovery here is liveliness (@redline/adv, /svc, /node).
set(Z_FEATURE_LIVELINESS 1 CACHE STRING "" FORCE)
set(Z_FEATURE_QUERY 1 CACHE STRING "" FORCE)
set(Z_FEATURE_QUERYABLE 0 CACHE STRING "" FORCE)
set(Z_FEATURE_SUBSCRIPTION 1 CACHE STRING "" FORCE)
set(Z_FEATURE_PUBLICATION 1 CACHE STRING "" FORCE)

set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_INTEGRATION OFF CACHE BOOL "" FORCE)
set(BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(ZENOHPICO_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(zenoh-pico)

# Check the source rather than trusting git apply's exit code, as zenoh-c.cmake
# does: without the ws patch everything still builds, and a refused or dropped
# WebSocket hangs the page instead of failing.
file(READ ${zenoh-pico_SOURCE_DIR}/src/link/transport/upper/ws_emscripten.c _pico_ws_c)
string(FIND "${_pico_ws_c}" "retry forever" _pico_patch_found)
if(_pico_patch_found EQUAL -1)
    message(FATAL_ERROR
        "zenoh_pico_emscripten_ws_closed_link.patch is not applied to "
        "${zenoh-pico_SOURCE_DIR}. Delete that directory and reconfigure.")
endif()

file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/licenses/zenoh-pico/patches)
file(COPY ${zenoh-pico_SOURCE_DIR}/LICENSE ${zenoh-pico_SOURCE_DIR}/README.md
     DESTINATION ${CMAKE_BINARY_DIR}/licenses/zenoh-pico)
file(COPY ${_pico_patch_paths} DESTINATION ${CMAKE_BINARY_DIR}/licenses/zenoh-pico/patches)

string(REPLACE ";" " " _pico_patch_list "${ZENOH_PICO_PATCHES}")
file(WRITE ${CMAKE_BINARY_DIR}/licenses/zenoh-pico/fetch_info.txt
"Library: zenoh-pico
Repository: https://github.com/eclipse-zenoh/zenoh-pico.git
Tag/Version: ${ZENOH_PICO_GIT_TAG}
Shallow Clone: TRUE
Patches Applied: ${_pico_patch_list}
")
