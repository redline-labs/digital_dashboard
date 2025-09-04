# Set deployment target for Rust builds (zenoh-c uses Cargo) before fetching
set(ENV{MACOSX_DEPLOYMENT_TARGET} ${CMAKE_OSX_DEPLOYMENT_TARGET})

# Fetch zenoh-c
FetchContent_Declare(
    zenohc
    GIT_REPOSITORY https://github.com/eclipse-zenoh/zenoh-c.git
    GIT_TAG 1.5.0
    GIT_SHALLOW TRUE
    OVERRIDE_FIND_PACKAGE
)

# Configure zenoh-c options
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(ZENOHC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZENOHC_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZENOHC_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(zenohc)

# Copy zenoh-c license
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/licenses/zenohc)
file(COPY ${zenohc_SOURCE_DIR}/LICENSE ${zenohc_SOURCE_DIR}/README.md
     DESTINATION ${CMAKE_BINARY_DIR}/licenses/zenohc)

# Write version info
file(WRITE ${CMAKE_BINARY_DIR}/licenses/zenohc/fetch_info.txt
"Library: zenoh-c
Repository: https://github.com/eclipse-zenoh/zenoh-c.git
Tag/Version: 1.5.0
Shallow Clone: TRUE
Patches Applied: None
")
