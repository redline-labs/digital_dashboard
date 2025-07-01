# Fetch hidapi
FetchContent_Declare(
    hidapi
    GIT_REPOSITORY https://github.com/libusb/hidapi.git
    GIT_TAG d6b2a97 # hidapi-0.15.0
    GIT_SHALLOW TRUE
)

# Configure hidapi options before making it available
# hidapi can use libusb as backend, so we need to ensure libusb is available
set(HIDAPI_WITH_LIBUSB ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(hidapi)

# Copy hidapi license
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/licenses/hidapi)
file(COPY ${hidapi_SOURCE_DIR}/LICENSE.txt ${hidapi_SOURCE_DIR}/LICENSE-bsd.txt 
     ${hidapi_SOURCE_DIR}/LICENSE-gpl3.txt ${hidapi_SOURCE_DIR}/LICENSE-orig.txt
     ${hidapi_SOURCE_DIR}/README.md
     DESTINATION ${CMAKE_BINARY_DIR}/licenses/hidapi)

# Write version info
file(WRITE ${CMAKE_BINARY_DIR}/licenses/hidapi/fetch_info.txt
"Library: hidapi
Repository: https://github.com/libusb/hidapi.git
Tag/Version: hidapi-0.15.0
Shallow Clone: TRUE
Patches Applied: None
") 