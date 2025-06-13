# Fetch zenoh-c
FetchContent_Declare(
    zenoh-c
    GIT_REPOSITORY https://github.com/eclipse-zenoh/zenoh-c.git
    GIT_TAG 1.4.0  # Latest stable release
    GIT_SHALLOW TRUE
)

# Configure zenoh-c options
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(ZENOHC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZENOHC_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZENOHC_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(zenoh-c)

# Copy zenoh-c license
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/licenses/zenoh-c)
file(COPY ${zenoh-c_SOURCE_DIR}/LICENSE ${zenoh-c_SOURCE_DIR}/README.md
     DESTINATION ${CMAKE_BINARY_DIR}/licenses/zenoh-c)

# Write version info
file(WRITE ${CMAKE_BINARY_DIR}/licenses/zenoh-c/fetch_info.txt
"Library: zenoh-c
Repository: https://github.com/eclipse-zenoh/zenoh-c.git
Tag/Version: v1.4.0 (Latest stable release)
Shallow Clone: TRUE
Patches Applied: None
") 