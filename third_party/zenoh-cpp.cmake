# Fetch zenoh-cpp
FetchContent_Declare(
    zenoh-cpp
    GIT_REPOSITORY https://github.com/eclipse-zenoh/zenoh-cpp.git
    GIT_TAG 1.9.0
    GIT_SHALLOW TRUE
)

# Configure zenoh-cpp options
set(ZENOHCXX_ZENOHC ON CACHE BOOL "" FORCE)
set(ZENOHCXX_ZENOHPICO OFF CACHE BOOL "" FORCE)
# Both spellings: zenoh-cpp renamed these from BUILD_ to ENABLE_ and the old
# names are silently ignored, so setting only those left its test suite
# registered with CTest -- a dozen tests we never build, reported as failures on
# every `ctest` run.
set(ZENOHCXX_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZENOHCXX_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZENOHCXX_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(ZENOHCXX_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZENOHCXX_INSTALL OFF CACHE BOOL "" FORCE)

# There used to be `set(Z_FEATURE_UNSTABLE_API ON CACHE BOOL "" FORCE)` here,
# which did nothing: no project in this build reads that name. The compile line
# for zenoh_pub_sub carried only -DZENOHCXX_ZENOHC, and the generated
# zenoh_configure.h had no Z_FEATURE_UNSTABLE_API -- so we believed the unstable
# API was on for two releases while `Bytes::get_contiguous_view()` and friends
# were quietly #ifdef'd out. Same shape as the CAPNP_EXECUTABLE variables
# described in capnproto.cmake: a setting that reads as enabled and is not.
#
# The knob that works is zenoh-c's, and it is a *build* option, not a define:
#
#     set(ZENOHC_BUILD_WITH_UNSTABLE_API ON CACHE BOOL "" FORCE)   # in zenoh-c.cmake
#
# which adds `--features=unstable` to the cargo invocation; the Rust build then
# writes `#define Z_FEATURE_UNSTABLE_API` into zenoh_configure.h, where both
# zenoh-c and zenoh-cpp pick it up. Left off deliberately: nothing here needs it
# (MatchingStatus and declare_background_matching_listener are stable as of
# zenoh-cpp 1.9), turning it on rebuilds the Rust library from scratch, and the
# zero-copy payload access we would want it for is available from the stable
# Bytes::slice_iter().
FetchContent_MakeAvailable(zenoh-cpp)

# Copy zenoh-cpp license
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/licenses/zenoh-cpp)
file(COPY ${zenoh-cpp_SOURCE_DIR}/LICENSE ${zenoh-cpp_SOURCE_DIR}/README.md
     DESTINATION ${CMAKE_BINARY_DIR}/licenses/zenoh-cpp)

# Write version info
file(WRITE ${CMAKE_BINARY_DIR}/licenses/zenoh-cpp/fetch_info.txt
"Library: zenoh-cpp
Repository: https://github.com/eclipse-zenoh/zenoh-cpp.git
Tag/Version: 1.9.0
Shallow Clone: TRUE
Patches Applied: None
") 