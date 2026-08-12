cmake_minimum_required(VERSION 3.10)

project(bd992)

# Talking to a Trimble BD992 over the network.
#
# The split from `gsof` is the same one `can` makes from `can_pcan`: bytes on
# one side, transports on the other. Everything here owns a socket, a thread or
# a file; nothing here knows a field offset. That is what lets the protocol half
# be constexpr and checked at compile time, and it is why this target may depend
# on `gsof` and `gsof` may never depend on this one.
#
# Deliberately free of zenoh and capnp too. The node maps records onto schemas;
# a library that knew what a `GsofLatLongHeight` was could not be tested without
# a bus.
add_library(bd992 STATIC
    src/control_client.cpp
    src/error.cpp
    src/output_config.cpp
    src/replay_stream.cpp
    src/stream_client.cpp
    src/tcp_stream.cpp
)

target_include_directories(bd992 PUBLIC include)

target_link_libraries(bd992
    PUBLIC
        gsof
    PRIVATE
        spdlog::spdlog
)

# The desired-versus-actual comparison that decides whether the node writes
# anything at all. Pure logic -- no sockets, no receiver -- which is the whole
# reason it was written as a free function over two lists.
add_executable(bd992_test_output_config
    tests/test_output_config.cpp
)

target_link_libraries(bd992_test_output_config
    PRIVATE
        bd992
        spdlog::spdlog
)

add_project_test(TARGET bd992_test_output_config LABELS bd992 unit)

# The stream and control paths, against a scripted receiver on the loopback
# interface. Labelled `net` because it opens sockets: no hardware, but not the
# hermetic `unit` set either.
add_executable(bd992_test_stream
    tests/test_stream.cpp
)

target_link_libraries(bd992_test_stream
    PRIVATE
        bd992
        spdlog::spdlog
)

add_project_test(TARGET bd992_test_stream LABELS bd992 net)
