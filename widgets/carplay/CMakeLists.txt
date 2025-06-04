# CarPlay Widget Library
cmake_minimum_required(VERSION 3.10)

# Define the library name
set(CARPLAY_WIDGET_LIB carplay_widget)

# Create the library with all the carplay source files
add_library(${CARPLAY_WIDGET_LIB} STATIC
    carplay_widget.cpp
    dongle_driver.cpp
    message.cpp
    opengl_shaders.cpp
)

# Set up include directories for this library
target_include_directories(${CARPLAY_WIDGET_LIB} PUBLIC
    include
    ${CMAKE_SOURCE_DIR}/include
    SYSTEM ${CMAKE_SOURCE_DIR}/yaml-cpp-0.8.0/include
    SYSTEM ${CMAKE_SOURCE_DIR}/libusb-cmake-1.0.27/libusb
    SYSTEM ${CMAKE_SOURCE_DIR}/cxxopts-3.2.0/include
    SYSTEM ${CMAKE_SOURCE_DIR}/nlohmann-json-3.11.3
)

# Find required packages for this widget
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBAV REQUIRED IMPORTED_TARGET
    libavcodec
    libswscale
    libavutil
)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets OpenGL OpenGLWidgets)

# Link required libraries to this widget
target_link_libraries(${CARPLAY_WIDGET_LIB} PUBLIC
    usb-1.0
    PkgConfig::LIBAV
    Qt6::Core
    Qt6::Widgets
    Qt6::OpenGL
    Qt6::OpenGLWidgets
    # Link spdlog (now available from parent CMakeLists.txt)
    spdlog::spdlog
)

# Set C++ standard
target_compile_features(${CARPLAY_WIDGET_LIB} PUBLIC cxx_std_20)

# Set AUTOMOC property for Qt
set_target_properties(${CARPLAY_WIDGET_LIB} PROPERTIES AUTOMOC ON)

# Also create the test_messages executable for this widget
add_executable(carplay_test_messages
    test_messages.cpp
    message.cpp
)

target_include_directories(carplay_test_messages PRIVATE
    include
    ${CMAKE_SOURCE_DIR}/include
    SYSTEM ${CMAKE_SOURCE_DIR}/yaml-cpp-0.8.0/include
    SYSTEM ${CMAKE_SOURCE_DIR}/nlohmann-json-3.11.3
)

target_compile_features(carplay_test_messages PRIVATE cxx_std_20)

# Link spdlog to the test executable too
target_link_libraries(carplay_test_messages PRIVATE
    spdlog::spdlog
) 