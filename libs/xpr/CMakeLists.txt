cmake_minimum_required(VERSION 3.10)

project(xpr)

# Talking to a MOTOTRBO radio over the network.
#
# The split from `mototrbo` is the one `bd992` makes from `gsof`: bytes on one
# side, transports on the other. Everything here owns a socket or a mutex;
# nothing here knows a field offset. That is what lets the protocol half be
# constexpr and checked at compile time, and it is why this target may depend
# on `mototrbo` and `mototrbo` may never depend on this one.
#
# Named for the radio, as bd992 is named for its receiver. The XPR 5550 is what
# this was validated against; the protocol is the whole MOTOTRBO family's.
#
# Deliberately free of zenoh and capnp. The node maps a channel or a display
# line onto a schema; a library that knew what an XprChannel was could not be
# tested without a bus.
add_library(xpr STATIC
    src/data_services.cpp
    src/error.cpp
    src/radio.cpp
    src/tcp_stream.cpp
)

target_include_directories(xpr PUBLIC include)

target_link_libraries(xpr
    PUBLIC
        mototrbo
    PRIVATE
        spdlog::spdlog
)

# The session end to end against a scripted radio in memory -- including the
# real TEA handshake, which is why this is a `unit` test rather than a `net`
# one. It is also where the five defects that only hardware could find are
# pinned: see the file's header comment.
add_executable(xpr_test_radio
    tests/test_radio.cpp
)

target_link_libraries(xpr_test_radio
    PRIVATE
        xpr
        mototrbo
        spdlog::spdlog
)

add_project_test(TARGET xpr_test_radio LABELS xpr unit)

# The data-service socket, against itself on the loopback interface. `net`
# rather than `unit`: it binds UDP ports.
add_executable(xpr_test_data_services
    tests/test_data_services.cpp
)

target_link_libraries(xpr_test_data_services
    PRIVATE
        xpr
        mototrbo
        spdlog::spdlog
)

add_project_test(TARGET xpr_test_data_services LABELS xpr net)
