cmake_minimum_required(VERSION 3.10)

project(display_backlight)

# What the rootfs says about a display module, and the sysfs behind it: the
# backlight class device, the IIO ambient light sensors and the hwmon
# temperature sensors.
#
# The rootfs (redline-display-setup, in the Yocto tree) detects the module at
# boot, binds the kernel drivers, and writes a record per display role to
# /run/redline/displays/<role>. This library reads that record and the files it
# points at. It does no detection of its own and never touches the backlight
# enable -- both belong to redline-display.
#
# No zenoh and no platform code: it is file I/O, so it builds and tests
# everywhere against a fake tree in a temp directory. Wiring it to the bus is
# nodes/backlight.
add_library(display_backlight STATIC
    src/display_record.cpp
    src/sysfs.cpp
    src/deadband.cpp
)

target_include_directories(display_backlight PUBLIC include)

# The record format, against the file a real boot wrote.
add_executable(display_backlight_test_record
    tests/test_display_record.cpp
)

target_link_libraries(display_backlight_test_record PRIVATE display_backlight)

add_project_test(TARGET display_backlight_test_record LABELS display_backlight unit)

# The sysfs readers and the brightness write, against a fake tree laid out the
# way the LattePanda's kernel lays out the real one.
add_executable(display_backlight_test_sysfs
    tests/test_sysfs.cpp
)

target_link_libraries(display_backlight_test_sysfs PRIVATE display_backlight)

add_project_test(TARGET display_backlight_test_sysfs LABELS display_backlight unit)

# When the node publishes ahead of its heartbeat: the lux and temperature bands
# at their boundaries, and the changes that always count.
add_executable(display_backlight_test_deadband
    tests/test_deadband.cpp
)

target_link_libraries(display_backlight_test_deadband PRIVATE display_backlight)

add_project_test(TARGET display_backlight_test_deadband LABELS display_backlight unit)
