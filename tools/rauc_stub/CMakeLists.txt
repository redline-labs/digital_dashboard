cmake_minimum_required(VERSION 3.10)

project(rauc_stub C)

# A fake RAUC on the session bus, so the console's reflash path can be developed
# and tested without a board. See the source for why this is C rather than the
# python3-gi version it replaces.
#
# A workstation tool: never shipped, and deliberately not declared with
# redline_install(). tools/ is added after the project's warning block on
# purpose, so this is held to the same -Werror as everything else.
find_package(PkgConfig REQUIRED)
pkg_check_modules(RAUC_STUB_GLIB REQUIRED IMPORTED_TARGET gio-2.0 gobject-2.0 glib-2.0)

add_executable(rauc_stub
    rauc_stub.c
)

target_link_libraries(rauc_stub PRIVATE PkgConfig::RAUC_STUB_GLIB)
