# The floating buttons both map surfaces overlay on their maps.
#
# A SIBLING of map_render rather than part of it, because these are QWidgets
# and map_render deliberately holds none -- see the note at the top of its
# CMakeLists. Everything here is chrome: the shared button base, the three
# glyphs, and the corner-stacking helper. The dashboard's map widget and
# scope's map panel both link it; neither may reach into the other's tree for
# a button again.
cmake_minimum_required(VERSION 3.16)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

# Q_OBJECT classes; without this the vtables and metaobjects never get
# generated and every use of findChild<...>() on these types fails to link.
set(CMAKE_AUTOMOC ON)

# The headers are LISTED as sources deliberately: AUTOMOC only scans headers
# it can pair with a source by directory or name, and these live under
# include/ where it does not look. Listing them is what gets their Q_OBJECT
# bodies generated.
add_library(map_controls STATIC
    src/map_button.cpp
    src/recentre_button.cpp
    src/compass_button.cpp
    src/view_mode_button.cpp
    src/zoom_button.cpp
    include/map_controls/map_button.h
    include/map_controls/recentre_button.h
    include/map_controls/compass_button.h
    include/map_controls/view_mode_button.h
    include/map_controls/zoom_button.h
)

target_include_directories(map_controls PUBLIC include)

target_link_libraries(map_controls
    PUBLIC
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
)
