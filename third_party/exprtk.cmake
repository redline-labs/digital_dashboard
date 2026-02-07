# Fetch exprtk
FetchContent_Declare(
    exprtk
    GIT_REPOSITORY https://github.com/ArashPartow/exprtk.git
    GIT_TAG 196a73144808a0039ea145095d7a55986dcf4435
    GIT_SHALLOW TRUE
)

# Configure exprtk options
# exprtk is header-only, so no specific build options needed

FetchContent_MakeAvailable(exprtk)

# Create an interface library target for exprtk
add_library(exprtk_lib INTERFACE)
target_include_directories(exprtk_lib SYSTEM INTERFACE "${exprtk_SOURCE_DIR}")

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
Tag/Version: 196a73144808a0039ea145095d7a55986dcf4435
Shallow Clone: TRUE
Patches Applied: None
")
