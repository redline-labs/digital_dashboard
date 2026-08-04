# Fetch capnproto
set(CAPNPROTO_GIT_TAG v1.5.0)

FetchContent_Declare(
    capnproto
    GIT_REPOSITORY https://github.com/capnproto/capnproto.git
    GIT_TAG ${CAPNPROTO_GIT_TAG}
    GIT_SHALLOW TRUE
)

# Configure capnproto options before making it available
# Disable building tests, examples, and tools we don't need
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(CAPNP_LITE OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(capnproto)

# Everything downstream refers to the compiler and the code generator by target
# name -- $<TARGET_FILE:capnp_tool> and $<TARGET_FILE:capnpc_cpp>, see
# schemas/CMakeLists.txt -- so there are no path variables to keep in sync here.
# There used to be: CAPNP_EXECUTABLE and friends, set from ${capnproto_SOURCE_DIR}
# *before* FetchContent_MakeAvailable() had defined it, so they expanded to
# nothing. Nothing read them, which is the only reason it did not matter.

# Copy capnproto license
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/licenses/capnproto)
file(COPY ${capnproto_SOURCE_DIR}/LICENSE ${capnproto_SOURCE_DIR}/README.md
     DESTINATION ${CMAKE_BINARY_DIR}/licenses/capnproto)

# Write version info. The tag comes from the variable rather than being written
# out again -- the literal here said v1.3.0 while we were fetching v1.5.0.
file(WRITE ${CMAKE_BINARY_DIR}/licenses/capnproto/fetch_info.txt
"Library: capnproto
Repository: https://github.com/capnproto/capnproto.git
Tag/Version: ${CAPNPROTO_GIT_TAG}
Shallow Clone: TRUE
Patches Applied: None
")
