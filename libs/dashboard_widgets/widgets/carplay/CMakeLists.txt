# CarPlay Widget Library
cmake_minimum_required(VERSION 3.10)

set(CARPLAY_WIDGET_LIB carplay_widget)

# Multimedia is used for PCM playback/mic capture of the driver's audio stream.
find_package(Qt6 REQUIRED COMPONENTS Core Widgets Multimedia Gui)

add_library(${CARPLAY_WIDGET_LIB} STATIC
    carplay_widget.cpp
    audio_ring.cpp
    include/carplay/carplay_widget.h
    include/carplay/audio_ring.h
    include/carplay/touch_slots.h
)

target_include_directories(${CARPLAY_WIDGET_LIB} PUBLIC
    ${DASHBOARD_WIDGET_INCLUDE_DIR}
    include
)

# libavcodec for software H.264/H.265 decode of the zenoh video stream, plus
# libavutil for the AVFrame/AVPacket allocators the decode loop calls directly
# and libswscale for the SIMD YUV420->BGRA conversion feeding QImage.
# The prefix is target-specific: PkgConfig::<prefix> is a global name, and
# nodes/carplay declares its own ffmpeg dependency.
find_package(PkgConfig REQUIRED)
pkg_check_modules(CARPLAY_WIDGET_LIBAV REQUIRED IMPORTED_TARGET
    libavcodec
    libavutil
    libswscale
)

target_link_libraries(${CARPLAY_WIDGET_LIB}
    PUBLIC
        PkgConfig::CARPLAY_WIDGET_LIBAV
        Qt6::Core
        Qt6::Widgets
        Qt6::Gui
        Qt6::Multimedia
        helpers
        helpers_ffmpeg
        config_codec
        reflection
        schemas
        zenoh_pub_sub
        zenohcxx::zenohc
    PRIVATE
        spdlog::spdlog
)

# Registered by hand rather than through add_dashboard_widget(): this widget
# needs ffmpeg via pkg-config and several extra Qt components, which is more
# than the shared helper should learn to express for one caller.
dashboard_widget_register(${CARPLAY_WIDGET_LIB})

set_target_properties(${CARPLAY_WIDGET_LIB} PROPERTIES AUTOMOC ON)

# When the return button shows: the whole truth table. Pure logic.
add_executable(carplay_test_return_button_policy test_return_button_policy.cpp)
target_include_directories(carplay_test_return_button_policy PRIVATE include)
add_project_test(TARGET carplay_test_return_button_policy LABELS dashboard unit)

# The same through a real widget and a real bus: the button appears with no
# session and goes once one is live, a tap sends one page command, and hiding the
# widget stops video and reports it. No phone or driver needed.
add_executable(carplay_test_return_button test_return_button.cpp)
target_link_libraries(carplay_test_return_button PRIVATE
    ${CARPLAY_WIDGET_LIB}
    spdlog::spdlog
)
add_project_test(TARGET carplay_test_return_button LABELS dashboard gui net)

# Throttling policy on its own, with time injected: exact interval boundaries
# and deferral state machine. No Qt, no zenoh, no threads, milliseconds to run.
add_executable(carplay_test_touch_throttle test_touch_throttle.cpp)
target_link_libraries(carplay_test_touch_throttle PRIVATE
    helpers
    spdlog::spdlog
)
add_project_test(TARGET carplay_test_touch_throttle LABELS dashboard unit)
target_include_directories(carplay_test_touch_throttle PRIVATE include)

# Finger-to-slot assignment for multitouch: stable slots, a third finger
# refused, lifts reported before the slot frees. Pure logic.
add_executable(carplay_test_touch_slots test_touch_slots.cpp)
target_link_libraries(carplay_test_touch_slots PRIVATE spdlog::spdlog)
target_include_directories(carplay_test_touch_slots PRIVATE include)
add_project_test(TARGET carplay_test_touch_slots LABELS dashboard unit)

# The same behaviour through a real widget and a real zenoh subscriber, which is
# what proves the policy is actually wired to the timer and the publisher. Runs
# headless; no phone or driver node needed, but it does spend real wall clock.
add_executable(carplay_test_touch_rate test_touch_rate.cpp)
target_link_libraries(carplay_test_touch_rate PRIVATE
    ${CARPLAY_WIDGET_LIB}
    spdlog::spdlog
)
# Drives a real widget and a real zenoh subscriber, and measures elapsed
# wall clock -- so it is gui, net and slow all at once.
add_project_test(TARGET carplay_test_touch_rate LABELS dashboard gui net slow)

# Multitouch through a real widget, Qt's real touch delivery and a real zenoh
# subscriber: a pinch, a third finger, the return button under a finger, and a
# page switch mid-touch. Needs QtTest for QTest::touchEvent, the only way to
# deliver touch the way a touchscreen does; skipped where it is not installed.
find_package(Qt6 QUIET COMPONENTS Test)
if(TARGET Qt6::Test)
    add_executable(carplay_test_multitouch test_multitouch.cpp)
    target_link_libraries(carplay_test_multitouch PRIVATE
        ${CARPLAY_WIDGET_LIB}
        Qt6::Test
        spdlog::spdlog
    )
    add_project_test(TARGET carplay_test_multitouch LABELS dashboard gui net)
endif()
