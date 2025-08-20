# Fetch capnproto
FetchContent_Declare(
    capnproto
    GIT_REPOSITORY https://github.com/capnproto/capnproto.git
    GIT_TAG 6846dff
    GIT_SHALLOW TRUE
)

# Configure capnproto options before making it available
# Disable building tests, examples, and tools we don't need
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(CAPNP_LITE OFF CACHE BOOL "" FORCE)

# We need the capnp compiler for schema compilation
set(CAPNP_INCLUDE_DIRS ${capnproto_SOURCE_DIR}/c++/src)
set(CAPNP_EXECUTABLE ${capnproto_BINARY_DIR}/c++/src/capnp/capnp)
set(CAPNPC_CXX_EXECUTABLE ${capnproto_BINARY_DIR}/c++/src/capnp/capnpc-c++)

FetchContent_MakeAvailable(capnproto)

# Copy capnproto license
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/licenses/capnproto)
file(COPY ${capnproto_SOURCE_DIR}/LICENSE ${capnproto_SOURCE_DIR}/README.md
     DESTINATION ${CMAKE_BINARY_DIR}/licenses/capnproto)

# Write version info
file(WRITE ${CMAKE_BINARY_DIR}/licenses/capnproto/fetch_info.txt
"Library: capnproto
Repository: https://github.com/capnproto/capnproto.git
Tag/Version: 6846dff
Shallow Clone: TRUE
Patches Applied: None
") 