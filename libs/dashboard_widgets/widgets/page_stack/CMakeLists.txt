add_dashboard_widget(page_stack
    SOURCES
        page_stack.cpp
        # Two Q_OBJECT classes, listed so AUTOMOC finds the header under include/.
        include/page_stack/page_stack.h
    PUBLIC_LIBS schemas
)

# Which page each command leads to. Pure logic, no Qt, no bus.
add_executable(page_stack_test_navigator test_navigator.cpp)
target_include_directories(page_stack_test_navigator PRIVATE include ${DASHBOARD_WIDGET_INCLUDE_DIR})
target_link_libraries(page_stack_test_navigator PRIVATE reflection)
add_project_test(TARGET page_stack_test_navigator LABELS page_stack unit)

# When a trigger's samples fire, with time injected.
add_executable(page_stack_test_trigger_edge test_trigger_edge.cpp)
target_include_directories(page_stack_test_trigger_edge PRIVATE include ${DASHBOARD_WIDGET_INCLUDE_DIR})
target_link_libraries(page_stack_test_trigger_edge PRIVATE reflection)
add_project_test(TARGET page_stack_test_trigger_edge LABELS page_stack unit)
