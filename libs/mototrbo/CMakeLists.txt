cmake_minimum_required(VERSION 3.10)

project(mototrbo)

# The MOTOTRBO protocols, and nothing else.
#
# The same split `gsof` makes from `bd992`: bytes on one side, transports on
# the other. Nothing here owns a socket, a thread or a file, and almost nothing
# here allocates -- the exceptions are the two functions that produce a
# std::string or a datagram, and they say so.
#
# The reason for the split is the same as gsof's, and it applies harder here
# because this protocol was reverse-engineered rather than published. Every way
# of getting a frame wrong -- a swapped address pair, a handshake payload two
# bytes short, a status reply read one byte off -- produces a frame the radio
# SILENTLY DROPS or a plausible wrong value, never an error. So the parsers and
# builders are constexpr and the tests assert them at build time against bytes
# a real XPR 5550 sent or accepted. If it compiles, the framing is the framing
# that worked on hardware.
#
# spdlog is deliberately absent, as it is from gsof: this library reports
# through Result<T>, and whether one refused command matters is the node's
# decision to make.
add_library(mototrbo STATIC
    src/control.cpp
    src/nai.cpp
)

target_include_directories(mototrbo PUBLIC include)

target_link_libraries(mototrbo)

# The frame layout and the TEA authentication. Mostly static_asserts; the
# executable exists for the cipher round trip, which needs a decrypt the
# library does not provide.
add_executable(mototrbo_test_xnl
    tests/test_xnl.cpp
)

target_include_directories(mototrbo_test_xnl PRIVATE tests)

target_link_libraries(mototrbo_test_xnl
    PRIVATE
        mototrbo
        spdlog::spdlog
)

add_project_test(TARGET mototrbo_test_xnl LABELS mototrbo unit)

# Channel control, radio status and the 0xB4xx broadcasts, against captured
# traffic. The display decode is the run-time half: it allocates.
add_executable(mototrbo_test_control
    tests/test_control.cpp
)

target_include_directories(mototrbo_test_control PRIVATE tests)

target_link_libraries(mototrbo_test_control
    PRIVATE
        mototrbo
        spdlog::spdlog
)

add_project_test(TARGET mototrbo_test_control LABELS mototrbo unit)

# The data services. Only the TMS half has ever been answered by a radio --
# see the provenance note in nai.h.
add_executable(mototrbo_test_nai
    tests/test_nai.cpp
)

target_include_directories(mototrbo_test_nai PRIVATE tests)

target_link_libraries(mototrbo_test_nai
    PRIVATE
        mototrbo
        spdlog::spdlog
)

add_project_test(TARGET mototrbo_test_nai LABELS mototrbo unit)
