# Fetch exprtk
FetchContent_Declare(
    exprtk
    GIT_REPOSITORY https://github.com/ArashPartow/exprtk.git
    GIT_TAG cc1b800  # Latest commit as of search
    GIT_SHALLOW TRUE
    PATCH_COMMAND git apply ${CMAKE_SOURCE_DIR}/patches/exprtk.patch
)

# Configure exprtk options
# exprtk is header-only, so no specific build options needed

FetchContent_MakeAvailable(exprtk)

# Create an interface library target for exprtk
add_library(exprtk_lib INTERFACE)
target_include_directories(exprtk_lib INTERFACE "${exprtk_SOURCE_DIR}")

# Add a namespaced alias for the exprtk target
add_library(exprtk::exprtk ALIAS exprtk_lib)

# Copy exprtk license
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/licenses/exprtk)
file(COPY ${exprtk_SOURCE_DIR}/license.txt ${exprtk_SOURCE_DIR}/readme.txt
     DESTINATION ${CMAKE_BINARY_DIR}/licenses/exprtk)

# Write version info
file(WRITE ${CMAKE_BINARY_DIR}/licenses/exprtk/fetch_info.txt
"Library: exprtk
Repository: https://github.com/ArashPartow/exprtk.git
Tag/Version: cc1b800 (Latest master)
Shallow Clone: TRUE
Patches Applied: exprtk.patch
")

# Copy exprtk patches
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/licenses/exprtk/patches)
file(COPY ${CMAKE_SOURCE_DIR}/patches/exprtk.patch
     DESTINATION ${CMAKE_BINARY_DIR}/licenses/exprtk/patches)