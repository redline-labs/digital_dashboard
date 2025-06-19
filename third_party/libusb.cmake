# Fetch libusb-cmake
FetchContent_Declare(
    libusb
    GIT_REPOSITORY https://github.com/libusb/libusb-cmake.git
    GIT_TAG v1.0.27
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(libusb)

# Copy libusb license
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/licenses/libusb)
file(COPY ${libusb_SOURCE_DIR}/LICENSE ${libusb_SOURCE_DIR}/README.md
     DESTINATION ${CMAKE_BINARY_DIR}/licenses/libusb)

# Write version info
file(WRITE ${CMAKE_BINARY_DIR}/licenses/libusb/fetch_info.txt
"Library: libusb
Repository: https://github.com/libusb/libusb-cmake.git
Tag/Version: v1.0.27
Shallow Clone: TRUE
Patches Applied: None
")
