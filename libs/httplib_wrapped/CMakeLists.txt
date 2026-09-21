cmake_minimum_required(VERSION 3.10)

project(httplib_wrapped)

# cpp-httplib behind one warning-suppression header. Header-only, so there is
# nothing to compile here -- the target exists to carry the include path and the
# httplib dependency to everyone who needs the HTTP types. The header itself
# explains why the indirection is required.
add_library(httplib_wrapped INTERFACE)

target_include_directories(httplib_wrapped INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(httplib_wrapped INTERFACE
    httplib::httplib
)
