include(FetchContent)

FetchContent_Declare(
    lexy
    GIT_REPOSITORY https://github.com/foonathan/lexy.git
    GIT_TAG v2025.05.0
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(lexy)

# Treat lexy's headers as system includes so its own -Wshadow / -Wold-style-cast
# noise does not surface in our builds.
foreach(lexy_target IN ITEMS _lexy_base lexy_core lexy_file lexy_unicode lexy_ext lexy)
    if(TARGET ${lexy_target})
        get_target_property(lexy_target_includes ${lexy_target} INTERFACE_INCLUDE_DIRECTORIES)
        if(lexy_target_includes)
            set_target_properties(${lexy_target} PROPERTIES
                INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${lexy_target_includes}"
            )
        endif()
    endif()
endforeach()

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