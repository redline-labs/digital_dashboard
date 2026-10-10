add_library(reflection INTERFACE)

target_include_directories(reflection INTERFACE
    include
)

# Field order, labels, nested structs and enum names: the list every config
# consumer walks.
add_executable(reflection_test_basics tests/test_basics.cpp)
target_link_libraries(reflection_test_basics PRIVATE reflection)
add_project_test(TARGET reflection_test_basics LABELS reflection unit)
