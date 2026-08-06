cmake_minimum_required(VERSION 3.10)

project(inspect)

# One file per verb, plus the traffic sampler they share.
#
# Adding a verb is: a file here, a declaration in include/inspect/verbs.h, and
# one row in the table in main.cpp. Everything else -- help text, the verb list,
# `--help` and `help <verb>`, exit codes, --debug/--json/--connect -- comes from
# libs/cli.
add_executable(inspect
    main.cpp

    # Discovery, from liveliness advertisements rather than from traffic.
    list.cpp
    info.cpp
    nodes.cpp
    services.cpp

    # Traffic.
    echo.cpp
    rate.cpp      # hz, bw and latency: one measurement, three reports.
    watch.cpp
    traffic.cpp

    # Neither: the registry is compiled in, so this one needs no bus at all.
    schema.cpp
    describe.cpp
    key_match.cpp

    # Writing to the bus. `call` lives in services.cpp, next to the discovery
    # that tells it what schema a request needs.
    publish.cpp
)

target_include_directories(inspect PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(inspect PRIVATE
    cli
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    zenohcxx::zenohc
    capnp
    schemas
    zenoh_pub_sub
)

# The wildcard matcher `list -k` filters with. Reimplemented rather than reused
# from zenoh (see inspect/key_match.h), so it has to earn that with a test --
# and its failure mode is a shorter list, which is indistinguishable from a bus
# with fewer publishers.
#
# `unit`: pure string logic, no session.
add_executable(inspect_test_key_match
    test_key_match.cpp
    key_match.cpp
)
target_include_directories(inspect_test_key_match PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
add_project_test(TARGET inspect_test_key_match LABELS inspect unit)
