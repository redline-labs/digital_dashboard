# Fetch cxxopts
FetchContent_Declare(
    cxxopts
    GIT_REPOSITORY https://github.com/jarro2783/cxxopts.git
    GIT_TAG 44380e5  # Version 3.3.1
    GIT_SHALLOW TRUE
)

# Configure cxxopts options
set(CXXOPTS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CXXOPTS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CXXOPTS_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(cxxopts)

# Copy cxxopts license
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/licenses/cxxopts)
file(COPY ${cxxopts_SOURCE_DIR}/LICENSE ${cxxopts_SOURCE_DIR}/README.md
     DESTINATION ${CMAKE_BINARY_DIR}/licenses/cxxopts)

# Write version info
file(WRITE ${CMAKE_BINARY_DIR}/licenses/cxxopts/fetch_info.txt
"Library: cxxopts
Repository: https://github.com/jarro2783/cxxopts.git
Tag/Version: 44380e5 (Version 3.3.1)
Shallow Clone: TRUE
Patches Applied: None
")
