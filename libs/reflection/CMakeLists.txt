add_library(reflection INTERFACE)

target_include_directories(reflection INTERFACE
    include
)

# Simple demo executable to exercise the reflection API
add_executable(test_reflection
    test_reflection.cpp
)

target_link_libraries(test_reflection
    reflection
)


