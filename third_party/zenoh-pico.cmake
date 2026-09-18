# zenoh-pico, for the BROWSER only.
#
# This file is deliberately NOT included from third_party/CMakeLists.txt. Nothing
# that runs on the board or a workstation should link pico: those get zenoh-c,
# which is the full Rust stack with shared memory and the gossip patch. Pico is
# here because a browser tab cannot be a zenoh peer -- no listening socket, no
# multicast, no raw TCP -- so the web console's wasm module talks `ws/` to zenohd
# as a zenoh CLIENT, and pico is the only implementation that compiles to wasm.
# wasm/CMakeLists.txt is the only consumer.
#
# PINNED AT 1.10.0 to match third_party/zenoh-c.cmake and zenoh-cpp.cmake.
#
# Note that zenoh-cpp vendors its own copy of pico (build/_deps/zenoh-cpp-src/
# zenoh-pico) and tracks it by BRANCH -- zenoh-pico-branch.txt says `main` -- and
# the copy that lands there is 1.9.0 even though zenoh-cpp itself is 1.10.0. That
# copy is unused here (ZENOHPICO is OFF in zenoh-cpp.cmake) and must not be
# reached for: a branch-tracked, version-skewed checkout is exactly the kind of
# thing this tree pins everything else to avoid.
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

# WebSocket is the only transport a browser has. zenoh-pico enforces the pairing
# itself -- CMakeLists.txt:363 hard-errors if Z_FEATURE_LINK_WS is on and the
# resolved system layer is not emscripten -- which is a useful guard rather than
# a nuisance: it means this file cannot accidentally be built for the target.
#
# ZP_PLATFORM, NOT ZP_SYSTEM_LAYER. 1.10.0 renamed the knob and made merely
# DEFINING the old name a fatal error (CMakeLists.txt:66), so it must be absent
# rather than renamed -- including as a stale cache entry from an earlier
# configure. The profile lives at cmake/platforms/emscripten.cmake and is what
# supplies src/link/transport/upper/ws_emscripten.c. zp_detect_default_platform()
# would infer it from CMAKE_SYSTEM_NAME anyway; it is named here so the build
# does not depend on that inference.
#
# (The zenoh-cpp-vendored copy of pico is 1.9.0 and still uses the old name. Do
# not read that copy to answer questions about this one -- that mistake cost a
# build.)
# Pico's own logging, so a stalled open says where it stalled rather than
# timing out silently. Matches upstream's emscripten CI, which builds with
# -DZENOH_LOG=debug.
set(ZENOH_LOG "debug" CACHE STRING "" FORCE)

set(ZP_PLATFORM "emscripten" CACHE STRING "" FORCE)

# zenohpico::lib aliases the SHARED library when BUILD_SHARED_LIBS is on, which
# is pico's default -- and cmake cannot build Emscripten shared libraries.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(Z_FEATURE_LINK_WS 1 CACHE STRING "" FORCE)
set(Z_FEATURE_LINK_TCP 0 CACHE STRING "" FORCE)
set(Z_FEATURE_LINK_UDP_UNICAST 0 CACHE STRING "" FORCE)
set(Z_FEATURE_LINK_UDP_MULTICAST 0 CACHE STRING "" FORCE)
set(Z_FEATURE_SCOUTING 0 CACHE STRING "" FORCE)

# Single-threaded, and the JS side pumps it. Emscripten pthreads mean
# SharedArrayBuffer, which means the page needs COOP/COEP headers, which means
# the web console has to serve them -- a large cost for a module whose only job
# is to decode. With MULTI_THREAD off there is no read task and no lease task, so
# the caller drives zp_read()/zp_send_keep_alive() from the event loop.
set(Z_FEATURE_MULTI_THREAD 0 CACHE STRING "" FORCE)
set(Z_FEATURE_MULTICAST_TRANSPORT 0 CACHE STRING "" FORCE)

# Discovery is liveliness-driven in this project (@redline/adv, @redline/svc,
# @redline/node), so the browser needs the liveliness API. Present since pico
# 1.1.0; verified in the 1.10.0 headers (z_liveliness_declare_subscriber,
# z_liveliness_declare_background_subscriber, z_liveliness_declare_token,
# z_liveliness_get).
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
