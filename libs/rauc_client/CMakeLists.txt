cmake_minimum_required(VERSION 3.10)

project(rauc_client)

# Driving RAUC over D-Bus, so the console can reflash the board.
#
# THE FIRST GLIB CONSUMER IN THE TREE. Nothing else links it, and RAUC's D-Bus
# API is the only interface it offers -- there is no library and no stable CLI
# output to parse -- so GDBus is the price of reflashing at all. The Yocto
# recipe carries a matching `DEPENDS += "glib-2.0"`; without it this fails at
# configure time in the cross build rather than at link time, which is the
# better of the two.
#
# No interface XML is generated against, deliberately: it ships in rauc-dev,
# which is not on the image, so the proxy is created by name and the signatures
# are written down in the header instead.
find_package(PkgConfig REQUIRED)
pkg_check_modules(RAUC_CLIENT_GLIB REQUIRED IMPORTED_TARGET gio-2.0 gobject-2.0 glib-2.0)

add_library(rauc_client STATIC
    src/rauc_client.cpp
)

target_include_directories(rauc_client PUBLIC include)

# glib is PRIVATE: the header deliberately exposes no GVariant, GDBusProxy or
# anything else from it, so a consumer -- the web console's update route --
# never sees glib at all.
target_link_libraries(rauc_client
    PUBLIC
        spdlog::spdlog
    PRIVATE
        PkgConfig::RAUC_CLIENT_GLIB
)

# Exercised against tools/rauc_stub/rauc_stub.py, which owns
# de.pengutronix.rauc on the SESSION bus. The test skips rather than fails when
# there is no bus or no stub, because a machine without one is not a broken
# machine -- see PROJECT_TEST_SKIP_CODE.
add_executable(rauc_client_test_installer
    tests/test_installer.cpp
)

target_link_libraries(rauc_client_test_installer PRIVATE
    rauc_client
    spdlog::spdlog
)

# The stub's path, the same way schemas_test_registry_plugin is handed capnp's.
# tools/ is added after libs/ in the root CMakeLists, so the target does not
# exist yet -- a generator expression is resolved at generate time and does not
# care, which is why this is not add_dependencies().
target_compile_definitions(rauc_client_test_installer PRIVATE
    RAUC_STUB="$<TARGET_FILE:rauc_stub>")

# All three of the stub's modes. The test starts its own stub, so a developer
# does not have to leave one running -- and RESOURCE_LOCK serialises them,
# because a well-known bus name cannot be shared and `ctest -j` would otherwise
# have them fighting over de.pengutronix.rauc.
add_project_test(TARGET rauc_client_test_installer NAME rauc_client_test_installer_ok
                 LABELS rauc_client unit ENVIRONMENT REDLINE_RAUC_STUB_MODE=ok)
add_project_test(TARGET rauc_client_test_installer NAME rauc_client_test_installer_fail
                 LABELS rauc_client unit ENVIRONMENT REDLINE_RAUC_STUB_MODE=fail)
add_project_test(TARGET rauc_client_test_installer NAME rauc_client_test_installer_busy
                 LABELS rauc_client unit ENVIRONMENT REDLINE_RAUC_STUB_MODE=busy)

set_tests_properties(
    rauc_client_test_installer_ok
    rauc_client_test_installer_fail
    rauc_client_test_installer_busy
    PROPERTIES RESOURCE_LOCK rauc_well_known_name)
