# Patches resolve against THIS file, not CMAKE_SOURCE_DIR; see spdlog.cmake.
get_filename_component(REDLINE_PATCH_DIR "${CMAKE_CURRENT_LIST_DIR}/../patches" ABSOLUTE)

# Fetch yaml-cpp
#
# Patched because its public headers pass a signed stream precision where a
# size_t is expected, and our code includes those headers with -Wsign-conversion
# on. The fix is upstream's own, from after 0.9.0.
FetchContent_Declare(
    yaml-cpp
    GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
    GIT_TAG yaml-cpp-0.9.0
    PATCH_COMMAND git apply ${REDLINE_PATCH_DIR}/yaml-cpp_precision_sign_conversion.patch
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
Tag/Version: yaml-cpp-0.9.0
Patches Applied: yaml-cpp_precision_sign_conversion.patch
")

# Copy yaml-cpp patches
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/licenses/yaml-cpp/patches)
file(COPY ${REDLINE_PATCH_DIR}/yaml-cpp_precision_sign_conversion.patch
     DESTINATION ${CMAKE_BINARY_DIR}/licenses/yaml-cpp/patches)
