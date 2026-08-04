cmake_minimum_required(VERSION 3.10)

project(can_backends)

# Assembles the backends into one registry. Sits above all of them so that
# `can` -- which they are written against -- does not have to know they exist.
add_library(can_backends STATIC
    src/registry.cpp
)

target_include_directories(can_backends PUBLIC include)

target_link_libraries(can_backends
    PUBLIC
        can
        can_pcan
        can_socketcan
)
