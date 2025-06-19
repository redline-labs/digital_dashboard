# Fetch nlohmann/json
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG 55f9368  # Version 3.12.0
    GIT_SHALLOW TRUE
)

# Configure nlohmann/json options
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(nlohmann_json)

# Copy nlohmann/json license
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/licenses/nlohmann_json)
file(COPY ${nlohmann_json_SOURCE_DIR}/LICENSE.MIT ${nlohmann_json_SOURCE_DIR}/README.md
     DESTINATION ${CMAKE_BINARY_DIR}/licenses/nlohmann_json)

# Write version info
file(WRITE ${CMAKE_BINARY_DIR}/licenses/nlohmann_json/fetch_info.txt
"Library: nlohmann/json
Repository: https://github.com/nlohmann/json.git
Tag/Version: 55f9368 (Version 3.12.0)
Shallow Clone: TRUE
Patches Applied: None
")
