# SPDX-License-Identifier: GPL-3.0-or-later

# Small Qt widget helpers shared by the GUI applications: a layered
# paint-caching QWidget base, helpers::Color -> QColor, and resource font
# loading.
#
# The Qt-using sibling of `helpers`, which is deliberately Qt-free. These were
# in dashboard/include/dashboard/ and nothing in them names a dashboard; the
# scope app draws its plots on CachedPaintWidget and resolves colours the same
# way.
#
# find_package is repeated here because imported targets are directory-scoped --
# the same reason libs/agent_control/CMakeLists.txt has to do it.
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

add_library(qt_helpers INTERFACE)

target_include_directories(qt_helpers INTERFACE
    include
)

target_link_libraries(qt_helpers INTERFACE
    helpers
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    spdlog::spdlog
)
