add_dashboard_widget(carplay_nav
    SOURCES carplay_nav.cpp
    PUBLIC_LIBS schemas
)

# The formatting rules are the only part of this widget that is not about
# pixels, so they are the part that gets a test. No Qt, no display, no bus.
add_executable(carplay_nav_test_format test_format.cpp)
target_link_libraries(carplay_nav_test_format PRIVATE carplay_nav)
add_project_test(TARGET carplay_nav_test_format LABELS carplay_nav unit)
