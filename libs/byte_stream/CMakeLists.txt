# The byte stream the BD992, MTi and XPR transports are written against, the
# TCP and replay streams under it, and the backoff their reconnect loops share.
#
# Each device library had its own copy of all of it, so a fix to connecting
# reached one of three. Kept free of every device, so linking it borrows an
# interface and nothing else.
add_library(byte_stream STATIC
    src/replay_stream.cpp
    src/tcp_stream.cpp
)

target_include_directories(byte_stream PUBLIC include)

# The schedule, including the case it exists for: a peer that accepts and
# closes at once is not reconnected in a tight loop.
add_executable(byte_stream_test_backoff tests/test_backoff.cpp)
target_link_libraries(byte_stream_test_backoff PRIVATE byte_stream)
add_project_test(TARGET byte_stream_test_backoff LABELS byte_stream unit)

# Opening streams that cannot be opened: no such file, an empty capture, no
# such host, a port nobody listens on. And a replay's chunking and looping.
add_executable(byte_stream_test_streams tests/test_streams.cpp)
target_link_libraries(byte_stream_test_streams PRIVATE byte_stream)
add_project_test(TARGET byte_stream_test_streams LABELS byte_stream net)
