# Fetch cpp-httplib
#
# The web console's HTTP server: the landing page, the RAUC reflash upload and
# the schema descriptors the browser's wasm module loads. Everything on the bus
# reaches the browser as zenoh over ws://; this serves the things that were
# never on the bus.
#
# Chosen over Crow, which was the starting preference, for one measurable
# reason: a RAUC bundle is ~338 MB and Crow's documented multipart handling
# buffers the body in memory. cpp-httplib's ContentReader hands the handler
# chunks as they arrive -- "keeps nothing, so the cap does not apply" -- so the
# bundle streams to /data and never exists in RAM. It also has binary WebSocket
# frames, SSE and TLS backends, all of which this console needs eventually.
set(CPP_HTTPLIB_GIT_TAG v0.56.0)

# Header-only -- httplib.h sits at the repository root -- but the repo's own
# CMakeLists builds a test suite and pulls dependencies to do it. SOURCE_SUBDIR
# pointed at a directory with no CMakeLists is the "populate but do not
# configure" trick from earcut.cmake and mcap.cmake. `example` is verified to
# have none at this tag; `test` is the only subdirectory that does, so do not
# point this there.
FetchContent_Declare(
    cpp_httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG ${CPP_HTTPLIB_GIT_TAG}
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR example
)

FetchContent_MakeAvailable(cpp_httplib)

add_library(cpp_httplib INTERFACE)

# NOT a SYSTEM include, for the reason the root CMakeLists spells out. The cost
# lands differently here than for the other vendored headers: httplib.h is
# 778 KB of header compiled inside OUR translation unit, so it meets -Werror
# with -Wconversion, -Wsign-conversion and -Wold-style-cast. That is handled at
# the one place that includes it -- nodes/web_console/httplib_include.h wraps it
# in a diagnostic push/ignored -- rather than by exempting the whole header and
# losing the warnings on our own code in the same file.
target_include_directories(cpp_httplib INTERFACE ${cpp_httplib_SOURCE_DIR})

add_library(httplib::httplib ALIAS cpp_httplib)

file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/licenses/cpp-httplib)
file(COPY ${cpp_httplib_SOURCE_DIR}/LICENSE ${cpp_httplib_SOURCE_DIR}/README.md
     DESTINATION ${CMAKE_BINARY_DIR}/licenses/cpp-httplib)

file(WRITE ${CMAKE_BINARY_DIR}/licenses/cpp-httplib/fetch_info.txt
"Library: cpp-httplib
Repository: https://github.com/yhirose/cpp-httplib.git
Tag/Version: ${CPP_HTTPLIB_GIT_TAG}
Shallow Clone: TRUE
Patches Applied: None
")
