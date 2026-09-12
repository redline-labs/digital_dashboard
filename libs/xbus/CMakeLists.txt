cmake_minimum_required(VERSION 3.10)

project(xbus)

# The Xsens XBus protocol, and nothing else.
#
# No serial ports, no threads, no zenoh, no allocation in the parse path.
# Everything an MTi says arrives as bytes, and every way of getting those bytes
# wrong -- a field offset, a fixed-point byte order, a data identifier's format
# nibble -- is a mistake that produces a plausible number rather than a
# failure. So this half is separated out and made constexpr: a byte vector plus
# a static_assert turns a wrong offset into a build error, which matters more
# here than it did for GSOF because there is no MTi on the bench to disagree.
#
# The transport half -- the serial port, the reader thread, the
# Config/Measurement handshake -- is `mti610`, which depends on this. Nothing
# here depends on that.
add_library(xbus STATIC
    src/framer.cpp
)

target_include_directories(xbus PUBLIC include)

# spdlog is deliberately absent, as it is in `gsof`. This library reports
# through Result<T> and counters; the decision about what is worth a log line
# belongs to the node, which knows whether one bad checksum an hour matters.
target_link_libraries(xbus)

# The message frame: the checksum rule, extended lengths, and resynchronisation
# after corruption. Holds the four byte vectors Xsens printed in the LLCP,
# which are the only bytes in this library that came from the vendor's encoder.
add_executable(xbus_test_framing
    tests/test_framing.cpp
)

target_include_directories(xbus_test_framing PRIVATE tests)

target_link_libraries(xbus_test_framing
    PRIVATE
        xbus
        spdlog::spdlog
)

add_project_test(TARGET xbus_test_framing LABELS xbus unit)

# The two fixed-point formats. The single most valuable test in this library:
# fp16.32's byte order cannot be checked by round-tripping, because an encoder
# and decoder that share the same mistake agree, so it is cross-checked against
# float32 -- a format with one legal layout and a vendor vector behind it.
add_executable(xbus_test_fixed_point
    tests/test_fixed_point.cpp
)

target_link_libraries(xbus_test_fixed_point
    PRIVATE
        xbus
        spdlog::spdlog
)

add_project_test(TARGET xbus_test_fixed_point LABELS xbus unit)

# The MTData2 body: the item walk and the fifteen payloads an MTi-610 emits.
# Every vector is synthetic -- see tests/golden/README.md -- so the assertion
# that carries weight is that one acceleration encoded in all four precisions
# parses to one answer.
add_executable(xbus_test_mtdata2
    tests/test_mtdata2.cpp
)

target_include_directories(xbus_test_mtdata2 PRIVATE tests)

target_link_libraries(xbus_test_mtdata2
    PRIVATE
        xbus
        spdlog::spdlog
)

add_project_test(TARGET xbus_test_mtdata2 LABELS xbus unit)

# The commands. Unlike the parsers these are checked against the LLCP's tables
# rather than against anything a device produced -- see the caveat at the top
# of the file, which is the same one libs/gsof's command test carries and which
# that library earned.
add_executable(xbus_test_commands
    tests/test_commands.cpp
)

target_include_directories(xbus_test_commands PRIVATE tests)

target_link_libraries(xbus_test_commands
    PRIVATE
        xbus
        spdlog::spdlog
)

add_project_test(TARGET xbus_test_commands LABELS xbus unit)
