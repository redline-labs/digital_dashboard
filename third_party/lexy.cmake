include(FetchContent)

FetchContent_Declare(
    lexy
    GIT_REPOSITORY https://github.com/foonathan/lexy.git
    GIT_TAG v2025.05.0
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(lexy)

file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/licenses/lexy)
file(COPY ${lexy_SOURCE_DIR}/LICENSE ${lexy_SOURCE_DIR}/LICENSE
     DESTINATION ${CMAKE_BINARY_DIR}/licenses/lexy)

# Write version info
file(WRITE ${CMAKE_BINARY_DIR}/licenses/lexy/fetch_info.txt
"Library: lexy
Repository: https://github.com/foonathan/lexy.git
Tag/Version: v2025.05.0
Shallow Clone: TRUE
Patches Applied: None
")