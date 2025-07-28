# Fetch exprtk
FetchContent_Declare(
    exprtk
    GIT_REPOSITORY https://github.com/ArashPartow/exprtk.git
    GIT_TAG cc1b800  # Latest commit as of search
    GIT_SHALLOW TRUE
)

# Configure exprtk options
# exprtk is header-only, so no specific build options needed

FetchContent_MakeAvailable(exprtk)

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
Patches Applied: None
") 