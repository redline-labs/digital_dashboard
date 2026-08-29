# SPDX-License-Identifier: GPL-3.0-or-later
#
# scope -- the live time-series visualizer.
#
# A sibling of dashboard/, not a subdirectory of it. It shares the reflected
# config machinery (config_codec) and the Qt widget helpers (qt_helpers) with
# the dashboard and the editor, but nothing else: dashboard widgets are not
# scope panels, so ${WIDGET_LIBS} is deliberately absent from the link line.

# Repeated here rather than inherited: find_package creates the Qt6:: imported
# targets in the calling directory scope only. Same reason
# libs/agent_control/CMakeLists.txt has to do it.
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

set(CMAKE_AUTOMOC ON)
qt_standard_project_setup()

# Everything except main(), so tests can link it. A test that had to link the
# executable would need main() out of the way, and the usual workarounds for
# that (a -DTESTING switch, a weak symbol) are worse than a library.
add_library(scope_core STATIC
    sample_ring.cpp
    raw_buffer.cpp
    decimate.cpp
    live_zenoh_source.cpp
    recorded_source.cpp
    capture_buffer.cpp
    scope_recorder.cpp
    time_base.cpp
    state_names.cpp
    panel_registry.cpp
    signal_browser.cpp
    add_signal_dialog.cpp
    panel_config_dialog.cpp
    overview_strip.cpp
    overview_controller.cpp
    transport_bar.cpp
    scope_window.cpp
    workspace.cpp
    settings.cpp
    settings_dialog.cpp
    scope_methods.cpp
    command_line_args.cpp

    # Headers carrying Q_OBJECT have to be listed as sources, because AUTOMOC
    # only looks for a matching header next to the .cpp that includes the moc
    # file -- and ours live under include/scope/. Leaving one out fails either
    # as "could not be found in the following directories" at generate time or,
    # worse, as a missing staticMetaObject at link time.
    include/scope/scope_window.h
    include/scope/time_base.h
    include/scope/panel.h
    include/scope/signal_browser.h
    include/scope/add_signal_dialog.h
    include/scope/overview_strip.h
    include/scope/settings_dialog.h
    include/scope/panel_config_dialog.h

)

set_target_properties(scope_core PROPERTIES AUTOMOC ON)

target_include_directories(scope_core PUBLIC include)

# One call registers a panel: its sources (INCLUDING any Q_OBJECT header --
# AUTOMOC only finds a header listed as a target source, and leaving one out
# fails as a missing staticMetaObject at link time) and its include/ directory.
# This used to be two separate edits in this file, and it was the one manual,
# error-prone step in the add-a-panel recipe panel_registry.h documents.
function(scope_add_panel dir)
    foreach(src IN LISTS ARGN)
        target_sources(scope_core PRIVATE panels/${dir}/${src})
    endforeach()
    target_include_directories(scope_core PUBLIC panels/${dir}/include)
endfunction()

# Panels. One directory per type, mirroring dashboard/widgets/ -- but built
# into scope_core rather than into a library each, because there is no
# equivalent of the widget palette's dynamic registration to serve and the
# panel table is resolved at compile time anyway.
scope_add_panel(time_series
    time_series_panel.cpp
    include/time_series/time_series_panel.h
)
scope_add_panel(video
    video_panel.cpp
    video_decoder.cpp
    video_decode_worker.cpp
    video_scrubber.cpp
    include/video/video_panel.h
    include/video/video_scrubber.h
)
scope_add_panel(table
    table_panel.cpp
    include/table/table_panel.h
)
scope_add_panel(map
    tile_reader.cpp
    track_builder.cpp
    map_panel.cpp
    include/map_panel/map_panel.h
)

# libavcodec for software H.264/H.265 decode of the recorded or live video
# stream, libavutil for the AVFrame/AVPacket allocators the decode loop calls
# directly, and libswscale for the YUV420->BGRA conversion feeding QImage.
#
# The prefix is target-specific because PkgConfig::<prefix> is a GLOBAL name and
# two other targets already declare their own ffmpeg dependency
# (dashboard/widgets/carplay and nodes/carplay). Reusing one of theirs would make
# the scope build depend on which of them CMake happened to configure first.
find_package(PkgConfig REQUIRED)
pkg_check_modules(SCOPE_LIBAV REQUIRED IMPORTED_TARGET
    libavcodec
    libavutil
    libswscale
)

target_link_libraries(scope_core PUBLIC
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets

    # The video panel's decoder, plus the shared ffmpeg->spdlog log routing.
    # Without the latter libavcodec and libswscale write straight to stderr,
    # untimed and unfiltered.
    PkgConfig::SCOPE_LIBAV
    helpers_ffmpeg

    agent_control
    zenoh_pub_sub

    # Reading a recording. The DataSource seam was designed for this: scope
    # links the library rather than shelling out to the `bag` binary, so a
    # recorded source is a DataSource implementation and nothing above it
    # changes.
    bag

    config_codec
    qt_helpers
    reflection
    helpers

    # Map archives, read straight off disk. THE DELIBERATE EXCEPTION to
    # "the vendored sqlite3 stays out of the GUI apps" (docs/map.md): that rule
    # protects the dashboard, which reaches SQLite through Qt6::Sql and must
    # never meet a second copy. scope is a desktop review tool that opens
    # archives itself and links no Qt6::Sql, so the two cannot meet here.
    mbtiles

    # Vector tiles to pixels: projection, tessellator, GPU renderer, label
    # layout, tile cache. Shared with dashboard/widgets/map, which gets the same
    # pixels from a different place -- it asks nodes/map_server, this reads the
    # file. Neither transport is visible from inside the library.
    map_render
    mvt

    spdlog::spdlog
    yaml-cpp::yaml-cpp
    cxxopts::cxxopts
    nlohmann_json::nlohmann_json
)

qt_add_executable(scope
    main.cpp
)

target_link_libraries(scope PRIVATE
    scope_core

    # Agent control interface (--mcp). Always built in; only listens when the
    # flag is supplied.
    agent_control
)

# ---------------------------------------------------------------------- tests

# The buffering layer on its own: the lock-free hand-off from the producer
# thread, the retained history, and the retention limits. No zenoh and no Qt, so
# this is a plain unit test and runs everywhere.
add_executable(scope_test_ring
    tests/test_sample_ring.cpp
)

target_link_libraries(scope_test_ring PRIVATE
    scope_core
)

add_project_test(TARGET scope_test_ring LABELS scope unit)

# The raw-payload buffer, which is the same two-bound argument CaptureBuffer
# makes applied one layer up -- plus the backwards-seek case, where a buffer that
# kept the position it came from stays perfectly ordered and is completely wrong.
add_executable(scope_test_raw_buffer
    tests/test_raw_buffer.cpp
)

target_link_libraries(scope_test_raw_buffer PRIVATE
    scope_core
)

add_project_test(TARGET scope_test_raw_buffer LABELS scope unit)

# The video decoder, against a stream the test ENCODES ITSELF with the same
# settings nodes/carplay/simulate.cpp uses. No fixture in the tree: a checked-in
# .h264 blob cannot rot against the ffmpeg the build actually links, and the
# pattern encodes its own frame index so a seek is checked on the pixels rather
# than on the decoder's own bookkeeping.
add_executable(scope_test_video_decoder
    tests/test_video_decoder.cpp
)

target_link_libraries(scope_test_video_decoder PRIVATE
    scope_core
    PkgConfig::SCOPE_LIBAV
)

add_project_test(TARGET scope_test_video_decoder LABELS scope unit)

# Per-column min/max reduction, against a brute-force reference over random
# data. Runs on every signal on every frame, and every way it can be wrong looks
# like data rather than like a bug.
add_executable(scope_test_decimate
    tests/test_decimate.cpp
)

target_link_libraries(scope_test_decimate PRIVATE
    scope_core
)

add_project_test(TARGET scope_test_decimate LABELS scope unit)

# Turning two signals into a line on a map, and finding a point on it.
#
# Every case draws SOMETHING when it is wrong: pairing by index skews the track
# over the wrong roads, interpolating the colour paints a speed nothing
# published, and a wrong hit test seeks somewhere else -- which reads as a
# mis-aimed mouse rather than as a bug.
add_executable(scope_test_map_track
    tests/test_map_track.cpp
)

target_link_libraries(scope_test_map_track PRIVATE
    scope_core
)

add_project_test(TARGET scope_test_map_track LABELS scope map unit)

# Tiles read straight off disk, against an archive this test writes itself.
#
# Every case is one that draws a plausible map rather than failing: the XYZ/TMS
# flip (a double flip renders beautifully, mirrored about the equator), absent
# versus failed, an unopenable archive kept with its reason, and the zoom range
# known at open.
add_executable(scope_test_map_tiles
    tests/test_map_tiles.cpp
)

target_link_libraries(scope_test_map_tiles PRIVATE
    scope_core
)

add_project_test(TARGET scope_test_map_tiles LABELS scope map unit)

# Per-user settings. Every case in here is one where the wrong behaviour looks
# like something else -- a first run reported as a broken install, a hand-edit
# with a typo silently replaced by an empty file, a duplicate name drawing a map
# from the wrong archive.
add_executable(scope_test_settings
    tests/test_settings.cpp
)

target_link_libraries(scope_test_settings PRIVATE
    scope_core
)

add_project_test(TARGET scope_test_settings LABELS scope unit)

# The workspace codec, including a byte-stability round trip over every shipped
# workspace. A codec that round-trips semantically but reorders keys turns every
# save into a diff, which makes a workspace impossible to keep in version
# control -- and that is exactly what someone will want to do with one.
add_executable(scope_test_workspace
    tests/test_workspace.cpp
)

# The shipped workspaces are the corpus, so the test has to find them wherever
# ctest happens to be run from. Same trick as editor_test_roundtrip.
target_compile_definitions(scope_test_workspace PRIVATE
    SCOPE_CONFIG_DIR="${CMAKE_SOURCE_DIR}/configs/scope")

target_link_libraries(scope_test_workspace PRIVATE
    scope_core
)

add_project_test(TARGET scope_test_workspace LABELS scope unit)

# Scrubbing a recording, over a stub provider with synthetic messages.
#
# The load-bearing case is the BACKWARDS seek: SampleHistory::lowerBound() is a
# binary search that assumes non-decreasing time and cannot detect when it is
# wrong, so a buffer left holding samples from two scrub positions produces a
# plausible wrong index and every reading taken from it is silently off.
# No Qt widgets and no zenoh, so this is a plain unit test.
add_executable(scope_test_recorded_source
    tests/test_recorded_source.cpp
)

target_link_libraries(scope_test_recorded_source PRIVATE
    scope_core
    # A real capnp payload, so the evaluator decodes it for real -- a
    # hand-rolled buffer would exercise the plumbing and none of the decoding.
    schemas
    capnp
    kj
)

add_project_test(TARGET scope_test_recorded_source LABELS scope unit)

# The in-memory capture's bounds and its eviction accounting.
#
# Both bounds matter and for different workloads: a video-carrying bus is
# OOM-killed within minutes under a time-only cap, and a telemetry-only one
# grows without limit under a byte-only cap. A buffer that honoured whichever
# bound its author expected would fail on the other, silently, in production.
add_executable(scope_test_capture_buffer
    tests/test_capture_buffer.cpp
)

target_link_libraries(scope_test_capture_buffer PRIVATE
    scope_core
)

add_project_test(TARGET scope_test_capture_buffer LABELS scope unit)

# The shared view window: zooming, panning, and what clamps it.
#
# Almost every way this can be wrong looks like data rather than like a bug -- a
# zoom that does not hold its anchor, a pan that leaves the buffers behind, a
# live view that clamps at the right edge and stops scrolling. None of them
# raise anything; they draw a plausible wrong picture, which is the same reason
# the decimation and scrubbing tests exist.
#
# QCoreApplication, no widgets and no bus, so a plain unit test.
qt_add_executable(scope_test_time_base
    tests/test_time_base.cpp
)

target_link_libraries(scope_test_time_base PRIVATE
    scope_core
)

add_project_test(TARGET scope_test_time_base LABELS scope unit)

# The window and panel layer against a real widget tree, so `gui` and offscreen.
# Uses a stub DataSource rather than the live zenoh one: everything here is
# widget behaviour, and needing a bus would make it a `net` test that skips
# itself on a machine without one.
qt_add_executable(scope_test_panels
    tests/test_panels.cpp
)

target_link_libraries(scope_test_panels PRIVATE
    scope_core
)

add_project_test(TARGET scope_test_panels LABELS scope gui)
