# SPDX-License-Identifier: GPL-3.0-or-later
#
# switchboard -- call the services advertised on the bus.
#
# A form-driven `inspect call`: every advertised service in a list, a form built
# at runtime from the selected one's request schema, and the reply as fields. The
# request build, the query and the reply decoding are pub_sub::callService --
# the same code `inspect call` uses -- so the two tools cannot disagree about
# what a request means or what came back.

# Repeated here rather than inherited: find_package creates the Qt6:: imported
# targets in the calling directory scope only. Same as scope/CMakeLists.txt.
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

set(CMAKE_AUTOMOC ON)
qt_standard_project_setup()

# Everything except main(), so tests can link it.
add_library(switchboard_core STATIC
    command_line_args.cpp
    schema_form.cpp
    response_view.cpp
    service_list.cpp
    call_history.cpp
    switchboard_window.cpp
    switchboard_methods.cpp

    # Q_OBJECT headers are listed as sources because AUTOMOC only finds a header
    # that is a target source when it lives under include/ -- leaving one out
    # fails as a missing staticMetaObject at link time.
    include/switchboard/schema_form.h
    include/switchboard/response_view.h
    include/switchboard/service_list.h
    include/switchboard/call_history.h
    include/switchboard/switchboard_window.h
)

set_target_properties(switchboard_core PROPERTIES AUTOMOC ON)

target_include_directories(switchboard_core PUBLIC include)

target_link_libraries(switchboard_core PUBLIC
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets

    agent_control
    zenoh_pub_sub
    schemas
    capnp

    spdlog::spdlog
    cxxopts::cxxopts
    nlohmann_json::nlohmann_json
)

qt_add_executable(switchboard
    main.cpp
)

target_link_libraries(switchboard PRIVATE
    core
    switchboard_core
    agent_control
)

# ---------------------------------------------------------------------- tests

# The runtime form builder. The load-bearing case is the sweep over EVERY schema
# in the registry: a form is built for each and what it produces must be
# accepted by jsonToCapnp, so a new schema with a field shape the form cannot
# represent fails here rather than when someone first selects that service.
# The rest runs against pub_sub's capnp_json fixture, which has every shape --
# unions, nested lists, Data, 64-bit extremes -- parsed at test time.
qt_add_executable(switchboard_test_schema_form
    tests/test_schema_form.cpp
)

target_compile_definitions(switchboard_test_schema_form PRIVATE
    FIXTURE_DIR="${CMAKE_SOURCE_DIR}/libs/pub_sub/test_fixtures")

target_link_libraries(switchboard_test_schema_form PRIVATE
    switchboard_core
    capnpc
    kj
)

add_project_test(TARGET switchboard_test_schema_form LABELS switchboard gui)

# The response pane over hand-built results: no bus needed to check that an
# ok=false reply gets its banner or that Data reads as grouped hex.
qt_add_executable(switchboard_test_response_view
    tests/test_response_view.cpp
)

target_link_libraries(switchboard_test_response_view PRIVATE
    switchboard_core
)

add_project_test(TARGET switchboard_test_response_view LABELS switchboard gui)

# The whole window against in-process services: discover, select, fill in,
# submit, read the reply, replay from history. Needs a session: `net` as well.
qt_add_executable(switchboard_test_window
    tests/test_window.cpp
)

target_link_libraries(switchboard_test_window PRIVATE
    switchboard_core
)

add_project_test(TARGET switchboard_test_window LABELS switchboard gui net)
