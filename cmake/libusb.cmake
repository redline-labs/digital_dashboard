# Fetch libusb-cmake
FetchContent_Declare(
    libusb
    GIT_REPOSITORY https://github.com/libusb/libusb-cmake.git
    GIT_TAG v1.0.27
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(libusb)
