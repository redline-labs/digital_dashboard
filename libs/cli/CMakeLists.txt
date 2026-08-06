# The shared command layer for this tree's multi-verb tools (inspect, bag).
#
# What it exists to stop: nodes/inspect had five verbs, each re-declaring
# -k/--key and -h/--help with different help strings, each re-parsing the whole
# argv, and a top-level help string maintained by hand in three places -- two of
# which were stale. Adding a verb touched four files. Here it is one table row.
add_library(cli STATIC
    program.cpp
    interrupt.cpp
    session_options.cpp
)

target_include_directories(cli PUBLIC
    include
)

target_link_libraries(cli
    PUBLIC
        # cxxopts is in the public interface: a Verb's add_options() callback
        # takes a cxxopts::Options&, so every consumer sees the type.
        cxxopts::cxxopts
    PRIVATE
        spdlog::spdlog
        core
        # Only session_options.cpp includes <zenoh.hxx>, which is why --connect
        # and --mode are handled in their own translation unit rather than in
        # program.cpp. See cli/session_options.h.
        zenoh_pub_sub
)

# Verb dispatch and option splitting, which is where every one of the old
# inspect's three defects lived: a bare invocation aborting, --debug rejected in
# both positions, and a missing required option reported as success.
#
# `unit` rather than `net`: nothing here opens a session. session_options.cpp
# links against SessionManager but only calls insertConfig(), which touches a
# vector and returns.
add_executable(cli_test_dispatch
    test_dispatch.cpp
)
target_link_libraries(cli_test_dispatch PRIVATE
    cli
    spdlog::spdlog
)
add_project_test(TARGET cli_test_dispatch LABELS cli unit)
