# Fetch zenoh-cpp
FetchContent_Declare(
    zenoh-cpp
    GIT_REPOSITORY https://github.com/eclipse-zenoh/zenoh-cpp.git
    GIT_TAG 1.4.0  # Latest stable release
    GIT_SHALLOW TRUE
)

# Configure zenoh-cpp options
set(ZENOHCXX_ZENOHC ON CACHE BOOL "" FORCE)
set(ZENOHCXX_ZENOHPICO OFF CACHE BOOL "" FORCE)
set(ZENOHCXX_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZENOHCXX_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZENOHCXX_INSTALL OFF CACHE BOOL "" FORCE)

set(Z_FEATURE_UNSTABLE_API ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(zenoh-cpp)

# Copy zenoh-cpp license
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/licenses/zenoh-cpp)
file(COPY ${zenoh-cpp_SOURCE_DIR}/LICENSE ${zenoh-cpp_SOURCE_DIR}/README.md
     DESTINATION ${CMAKE_BINARY_DIR}/licenses/zenoh-cpp)

# Write version info
file(WRITE ${CMAKE_BINARY_DIR}/licenses/zenoh-cpp/fetch_info.txt
"Library: zenoh-cpp
Repository: https://github.com/eclipse-zenoh/zenoh-cpp.git
Tag/Version: 1.4.0 (Latest stable release)
Shallow Clone: TRUE
Patches Applied: None
") 