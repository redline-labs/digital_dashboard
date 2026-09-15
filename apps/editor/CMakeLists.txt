# SPDX-License-Identifier: GPL-3.0-or-later
#
# editor -- the layout editor for the dashboard.
#
# A sibling of apps/dashboard, not a second executable inside it. The two share
# libs/dashboard_widgets -- the widgets, the layout config, the factory -- and
# nothing else: the dashboard never sees an editor header, and the editor
# reaches the dashboard's config only through that library's public interface.

# Repeated here rather than inherited: find_package creates the Qt6:: imported
# targets in the calling directory scope only. Same as apps/scope/CMakeLists.txt.
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

set(CMAKE_AUTOMOC ON)
qt_standard_project_setup()

# Everything except main(), so tests can link it.
add_library(editor_core STATIC
    editor_window.cpp
    widget_palette.cpp
    canvas.cpp
    editor_document.cpp
    properties_panel.cpp
    selection_frame.cpp
    editor_methods.cpp

    # Q_OBJECT headers are listed as sources because AUTOMOC only finds a header
    # that is a target source when it lives under include/ -- leaving one out
    # fails as a missing staticMetaObject at link time.
    include/editor/canvas.h
    include/editor/editor_window.h
    include/editor/properties_panel.h
    include/editor/selection_frame.h
    include/editor/widget_palette.h
)

set_target_properties(editor_core PROPERTIES AUTOMOC ON)

target_include_directories(editor_core PUBLIC include)

target_link_libraries(editor_core PUBLIC
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets

    dashboard_widgets
    agent_control
    zenoh_pub_sub
    helpers
    config_codec
    qt_helpers

    spdlog::spdlog
    yaml-cpp::yaml-cpp
    nlohmann_json::nlohmann_json
)

qt_add_executable(editor
    main.cpp
)

target_link_libraries(editor PRIVATE
    core
    editor_core
    agent_control
    cxxopts::cxxopts
)

# macOS: the app bundle displays this name in the menu bar.
if (APPLE)
    set_target_properties(editor PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Dashboard Editor"
    )
endif()

# ---------------------------------------------------------------------- tests

# The undo/redo history on its own, against a fake document. No widgets, so
# unlike the roundtrip test below it needs no display and no QApplication.
add_executable(editor_test_document
    tests/test_editor_document.cpp
)
target_link_libraries(editor_test_document PRIVATE editor_core)
add_project_test(TARGET editor_test_document LABELS editor unit)

# The editor's own load/edit/export path, above the codec. Constructs real
# widgets, so it is a gui test and runs offscreen.
qt_add_executable(editor_test_roundtrip
    tests/test_editor_roundtrip.cpp
)
# The shipped configs are the corpus for the byte-stability check, so the test
# has to find them wherever ctest happens to be run from.
target_compile_definitions(editor_test_roundtrip PRIVATE
    DASHBOARD_CONFIG_DIR="${CMAKE_SOURCE_DIR}/configs/dashboard")
target_link_libraries(editor_test_roundtrip PRIVATE editor_core)
add_project_test(TARGET editor_test_roundtrip LABELS editor gui)
