# Fetch yaml-cpp
FetchContent_Declare(
    yaml-cpp
    GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
    GIT_TAG aa8d4e4
    GIT_SHALLOW TRUE
)

# Configure yaml-cpp options
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(YAML_CPP_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(yaml-cpp)

# Copy yaml-cpp license
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/licenses/yaml-cpp)
file(COPY ${yaml-cpp_SOURCE_DIR}/LICENSE ${yaml-cpp_SOURCE_DIR}/README.md
     DESTINATION ${CMAKE_BINARY_DIR}/licenses/yaml-cpp)

# Write version info
file(WRITE ${CMAKE_BINARY_DIR}/licenses/yaml-cpp/fetch_info.txt
"Library: yaml-cpp
Repository: https://github.com/jbeder/yaml-cpp.git
Tag/Version: aa8d4e4
Shallow Clone: TRUE
Patches Applied: None
")
