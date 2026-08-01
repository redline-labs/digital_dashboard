# libimobiledevice, vendored.
#
# Why vendored rather than taken from the distro: we depend on version-specific
# behaviour (libusbmuxd's 24 -> 25 character dashed UDID normalisation, and
# 1.4.0's lack of an environment override for its on-disk pair-record fallback,
# both noted in libs/apple_usb/lockdown.cpp), and the distro version is whatever
# the build host happens to ship. Debian stable is still on 1.3.0.
#
# Why our own CMakeLists rather than the upstream build: the family is
# autotools, so consuming it natively would mean ExternalProject_Add plus the
# autotools chain on every build host. We compile the handful of translation
# units our surface actually needs instead. That also sidesteps libtatsu, which
# configure.ac requires but which only tools/ideviceimagemounter.c includes --
# no library source references it.
#
# What we use it for is small in call count but deep in behaviour: the lockdown
# handshake (pairing -- RSA keygen and X.509 generation -- plus StartSession and
# the TLS session it enables). See docs/carplay_bringup.md for why that is not
# worth reimplementing.
#
# All four are LGPL-2.1+, compatible with this project's GPL-3.

include(CheckSymbolExists)
include(CheckStructHasMember)
include(CheckIncludeFile)

# These four are pinned as a set, not independently. libimobiledevice 1.3.0
# against libplist 2.6.0 does not compile: 1.3.0 carries its own
# `enum plist_format_t` in common/utils.h, which libplist later absorbed into
# plist.h, and the enumerators collide. 1.4.0 is also the version
# libs/apple_usb/lockdown.cpp documents its behaviour against.
set(LIMD_PLIST_TAG 2.6.0)
set(LIMD_GLUE_TAG 1.3.1)
set(LIMD_USBMUXD_TAG 2.1.0)
set(LIMD_TAG 1.4.0)

FetchContent_Declare(vendored_libplist
    GIT_REPOSITORY https://github.com/libimobiledevice/libplist.git
    GIT_TAG ${LIMD_PLIST_TAG}
    GIT_SHALLOW TRUE
)
FetchContent_Declare(vendored_limd_glue
    GIT_REPOSITORY https://github.com/libimobiledevice/libimobiledevice-glue.git
    GIT_TAG ${LIMD_GLUE_TAG}
    GIT_SHALLOW TRUE
)
FetchContent_Declare(vendored_libusbmuxd
    GIT_REPOSITORY https://github.com/libimobiledevice/libusbmuxd.git
    GIT_TAG ${LIMD_USBMUXD_TAG}
    GIT_SHALLOW TRUE
)
FetchContent_Declare(vendored_limd
    GIT_REPOSITORY https://github.com/libimobiledevice/libimobiledevice.git
    GIT_TAG ${LIMD_TAG}
    GIT_SHALLOW TRUE
)

# Populate only -- none of these have a CMakeLists for us to add_subdirectory.
FetchContent_MakeAvailable(vendored_libplist vendored_limd_glue vendored_libusbmuxd vendored_limd)

# --- feature probes, standing in for configure ------------------------------
check_symbol_exists(gettimeofday "sys/time.h" HAVE_GETTIMEOFDAY)
check_symbol_exists(localtime_r "time.h" HAVE_LOCALTIME_R)
check_symbol_exists(gmtime_r "time.h" HAVE_GMTIME_R)
check_symbol_exists(strptime "time.h" HAVE_STRPTIME)
check_symbol_exists(getifaddrs "ifaddrs.h" HAVE_GETIFADDRS)
check_symbol_exists(poll "poll.h" HAVE_POLL)
check_symbol_exists(pselect "sys/select.h" HAVE_PSELECT)
check_symbol_exists(pthread_cancel "pthread.h" HAVE_PTHREAD_CANCEL)
check_include_file("sys/types.h" HAVE_SYS_TYPES_H)
check_struct_has_member("struct tm" tm_gmtoff "time.h" HAVE_TM_TM_GMTOFF)
check_struct_has_member("struct tm" tm_zone "time.h" HAVE_TM_TM_ZONE)

# These are glibc extensions. _GNU_SOURCE has to be set for the probe or they
# come back false on Linux, which is the platform that actually has them.
set(CMAKE_REQUIRED_DEFINITIONS -D_GNU_SOURCE)
check_symbol_exists(memmem "string.h" HAVE_MEMMEM)
check_symbol_exists(strndup "string.h" HAVE_STRNDUP)
check_symbol_exists(stpcpy "string.h" HAVE_STPCPY)
check_symbol_exists(stpncpy "string.h" HAVE_STPNCPY)
check_symbol_exists(asprintf "stdio.h" HAVE_ASPRINTF)
check_symbol_exists(vasprintf "stdio.h" HAVE_VASPRINTF)
check_symbol_exists(program_invocation_short_name "errno.h"
                    HAVE_PROGRAM_INVOCATION_SHORT_NAME_ERRNO_H)
unset(CMAKE_REQUIRED_DEFINITIONS)

if(HAVE_PROGRAM_INVOCATION_SHORT_NAME_ERRNO_H)
    set(HAVE_PROGRAM_INVOCATION_SHORT_NAME 1)
endif()
if(HAVE_PTHREAD_CANCEL)
    set(HAVE_THREAD_CLEANUP 1)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    check_include_file("sys/inotify.h" HAVE_INOTIFY)
endif()

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

# Generates a per-library config.h. Separate copies because PACKAGE_VERSION is
# baked into each library's own reporting.
function(_limd_write_config target pkg_name pkg_version out_dir_var)
    set(LIMD_PKG_NAME "${pkg_name}")
    set(LIMD_PKG_VERSION "${pkg_version}")
    set(_dir "${CMAKE_BINARY_DIR}/third_party/limd_config/${target}")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/libimobiledevice/config.h.in"
        "${_dir}/config.h"
        @ONLY
    )
    set(${out_dir_var} "${_dir}" PARENT_SCOPE)
endfunction()

# --- libplist ----------------------------------------------------------------
# C sources only; the C++ bindings (Array.cpp and friends) are a separate
# library upstream and nothing here uses them.
#
# These lists track the pinned tags above, taken from each project's
# Makefile.am at that tag -- they are not stable across versions (master has
# since added src/common.c to libplist, for one). Re-derive them when bumping.
_limd_write_config(libplist libplist ${LIMD_PLIST_TAG} _plist_config_dir)
add_library(vendored_plist STATIC
    ${vendored_libplist_SOURCE_DIR}/src/base64.c
    ${vendored_libplist_SOURCE_DIR}/src/bytearray.c
    ${vendored_libplist_SOURCE_DIR}/src/hashtable.c
    ${vendored_libplist_SOURCE_DIR}/src/ptrarray.c
    ${vendored_libplist_SOURCE_DIR}/src/time64.c
    ${vendored_libplist_SOURCE_DIR}/src/xplist.c
    ${vendored_libplist_SOURCE_DIR}/src/bplist.c
    ${vendored_libplist_SOURCE_DIR}/src/jsmn.c
    ${vendored_libplist_SOURCE_DIR}/src/jplist.c
    ${vendored_libplist_SOURCE_DIR}/src/oplist.c
    ${vendored_libplist_SOURCE_DIR}/src/out-default.c
    ${vendored_libplist_SOURCE_DIR}/src/out-plutil.c
    ${vendored_libplist_SOURCE_DIR}/src/out-limd.c
    ${vendored_libplist_SOURCE_DIR}/src/plist.c
    # libcnary is libplist's own vendored node/list container, built into the
    # same library upstream rather than being an external dependency.
    ${vendored_libplist_SOURCE_DIR}/libcnary/node.c
    ${vendored_libplist_SOURCE_DIR}/libcnary/node_list.c
)
target_include_directories(vendored_plist
    PUBLIC ${vendored_libplist_SOURCE_DIR}/include
    PRIVATE
        ${vendored_libplist_SOURCE_DIR}/src
        ${vendored_libplist_SOURCE_DIR}/libcnary/include
        ${_plist_config_dir}
)
target_compile_definitions(vendored_plist PRIVATE HAVE_CONFIG_H _GNU_SOURCE)

# --- libimobiledevice-glue ---------------------------------------------------
# Only the units our two consumers actually include: utils/socket/thread for
# libimobiledevice, plus collection for libusbmuxd's device_monitor (its
# listener and device lists). The rest of upstream's source list -- opack, tlv,
# the sha implementations, nskeyedarchive, termcolors -- backs services we do
# not build.
_limd_write_config(limd_glue libimobiledevice-glue ${LIMD_GLUE_TAG} _glue_config_dir)
add_library(vendored_limd_glue_lib STATIC
    ${vendored_limd_glue_SOURCE_DIR}/src/utils.c
    ${vendored_limd_glue_SOURCE_DIR}/src/socket.c
    ${vendored_limd_glue_SOURCE_DIR}/src/thread.c
    ${vendored_limd_glue_SOURCE_DIR}/src/collection.c
)
target_include_directories(vendored_limd_glue_lib
    PUBLIC ${vendored_limd_glue_SOURCE_DIR}/include
    PRIVATE ${vendored_limd_glue_SOURCE_DIR}/src ${_glue_config_dir}
)
target_compile_definitions(vendored_limd_glue_lib PRIVATE HAVE_CONFIG_H _GNU_SOURCE)
target_link_libraries(vendored_limd_glue_lib PUBLIC vendored_plist Threads::Threads)

# --- libusbmuxd --------------------------------------------------------------
# A single translation unit; this is the client of the mux socket, and the
# reason we can point it at our own UsbmuxdServer with USBMUXD_SOCKET_ADDRESS
# rather than the system daemon.
_limd_write_config(libusbmuxd libusbmuxd ${LIMD_USBMUXD_TAG} _muxd_config_dir)
add_library(vendored_usbmuxd STATIC
    ${vendored_libusbmuxd_SOURCE_DIR}/src/libusbmuxd.c
)
target_include_directories(vendored_usbmuxd
    PUBLIC ${vendored_libusbmuxd_SOURCE_DIR}/include
    PRIVATE ${vendored_libusbmuxd_SOURCE_DIR}/src ${_muxd_config_dir}
)
target_compile_definitions(vendored_usbmuxd PRIVATE HAVE_CONFIG_H _GNU_SOURCE)
target_link_libraries(vendored_usbmuxd PUBLIC vendored_plist vendored_limd_glue_lib)

# --- libimobiledevice --------------------------------------------------------
# The subset backing our surface: idevice_new_with_options / idevice_connect /
# idevice_connection_enable_ssl / idevice_disconnect / idevice_free, and the
# lockdownd_* handshake, start_service and teardown calls. Everything else in
# src/ is a per-service client (afc, installation_proxy, syslog_relay, ...) that
# we never touch.
#
# common/userpref.c is the pair-record store: RSA keygen and X.509 generation
# for the host/root/device certificates. It is the reason this dependency is
# worth keeping rather than reimplementing.
_limd_write_config(limd libimobiledevice ${LIMD_TAG} _limd_config_dir)
add_library(vendored_imobiledevice STATIC
    ${vendored_limd_SOURCE_DIR}/src/idevice.c
    ${vendored_limd_SOURCE_DIR}/src/lockdown.c
    ${vendored_limd_SOURCE_DIR}/src/property_list_service.c
    ${vendored_limd_SOURCE_DIR}/src/service.c
    ${vendored_limd_SOURCE_DIR}/common/userpref.c
    ${vendored_limd_SOURCE_DIR}/common/debug.c
)
target_include_directories(vendored_imobiledevice
    PUBLIC ${vendored_limd_SOURCE_DIR}/include
    PRIVATE
        ${vendored_limd_SOURCE_DIR}
        ${vendored_limd_SOURCE_DIR}/src
        ${vendored_limd_SOURCE_DIR}/common
        ${_limd_config_dir}
)
target_compile_definitions(vendored_imobiledevice PRIVATE HAVE_CONFIG_H _GNU_SOURCE)
target_link_libraries(vendored_imobiledevice
    PUBLIC
        vendored_plist
        vendored_usbmuxd
        vendored_limd_glue_lib
        OpenSSL::SSL
        OpenSSL::Crypto
        Threads::Threads
)

# Upstream builds with -Werror and a specific warning set; we are compiling
# their sources under this project's flags, which are stricter in places they
# never targeted. Their warnings are not ours to fix.
foreach(_t vendored_plist vendored_limd_glue_lib vendored_usbmuxd vendored_imobiledevice)
    target_compile_options(${_t} PRIVATE -w)
    set_target_properties(${_t} PROPERTIES C_CLANG_TIDY "" C_CPPCHECK "")
endforeach()

# One umbrella target so consumers do not depend on the internal split.
add_library(limd INTERFACE)
target_link_libraries(limd INTERFACE vendored_imobiledevice)

# --- licenses ----------------------------------------------------------------
foreach(_pair "libplist@${vendored_libplist_SOURCE_DIR}@${LIMD_PLIST_TAG}"
              "libimobiledevice-glue@${vendored_limd_glue_SOURCE_DIR}@${LIMD_GLUE_TAG}"
              "libusbmuxd@${vendored_libusbmuxd_SOURCE_DIR}@${LIMD_USBMUXD_TAG}"
              "libimobiledevice@${vendored_limd_SOURCE_DIR}@${LIMD_TAG}")
    string(REPLACE "@" ";" _parts "${_pair}")
    list(GET _parts 0 _name)
    list(GET _parts 1 _src)
    list(GET _parts 2 _tag)
    file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/licenses/${_name})
    file(GLOB _lic ${_src}/COPYING ${_src}/COPYING.LESSER ${_src}/README.md)
    if(_lic)
        file(COPY ${_lic} DESTINATION ${CMAKE_BINARY_DIR}/licenses/${_name})
    endif()
    file(WRITE ${CMAKE_BINARY_DIR}/licenses/${_name}/fetch_info.txt
"Library: ${_name}
Repository: https://github.com/libimobiledevice/${_name}.git
Tag/Version: ${_tag}
Shallow Clone: TRUE
Patches Applied: None
Note: built from a subset of upstream sources via our own CMakeLists
      (third_party/libimobiledevice.cmake), not the upstream autotools build.
")
endforeach()
