# The zenoh router, built from the workspace zenoh-c.cmake already fetches.
#
# WHY THERE IS NO SEPARATE FETCH HERE. zenohd is a workspace member of the same
# eclipse-zenoh/zenoh checkout that third_party/zenoh-c.cmake pins to
# release/1.10.0, SHA-verifies against zenoh-c's lockfile, and patches with
# patches/zenoh_abortable_gossip_connect.patch. Building it from that tree rather
# than from crates.io is what makes router/library drift IMPOSSIBLE rather than
# merely discouraged: they are the same source, the same revision and the same
# patch, or the build fails in zenoh-c.cmake before reaching here.
#
# Verified 2026-09-17: the resulting binary reports `zenohd v1.10.0-modified` --
# git-version detecting the patched worktree -- and opens both a tcp and a ws
# listener (confirmed in the kernel socket table, not just its own log).
#
# transport_ws needs no flag: it is in the `zenoh` crate's default feature set,
# and zenohd's own default is ["zenoh/default"]. The ws listener is what the web
# console's browser client connects to; a browser cannot be a zenoh peer.
#
# This file must be included AFTER zenoh-c.cmake, which is what defines
# zenoh_rust_SOURCE_DIR.

# redline_install_program() lives here. Included by a path relative to this
# file, not by module name: third_party is configured at root line 68, and the
# root does not include(ProjectInstall) until line ~127.
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/ProjectInstall.cmake)

if(NOT DEFINED zenoh_rust_SOURCE_DIR)
    message(FATAL_ERROR
        "zenohd.cmake needs zenoh_rust_SOURCE_DIR, which zenoh-c.cmake defines. "
        "Include it after zenoh-c.cmake.")
endif()

# OFF by default, and that is a deliberate deviation from "it falls out of the
# build you already run". It does -- but it is a 300+ crate cargo build, and a
# developer running the cluster on a laptop has no use for a router. The image
# turns it on (EXTRA_OECMAKE in redline-dashboard_1.0.bb); nobody else pays for
# it. Turning it on changes nothing about WHERE it is built, only whether.
option(REDLINE_BUILD_ZENOHD "Build the zenoh router from the vendored workspace" OFF)

if(REDLINE_BUILD_ZENOHD)
    find_program(REDLINE_CARGO cargo REQUIRED)

    # A target directory of its own. Sharing zenoh-c's would buy almost nothing:
    # zenohd resolves `zenoh` with internal/plugins/runtime_plugins/unstable on
    # top of the defaults, cargo keys artifacts on the exact feature set, and the
    # two are separate workspaces where no feature unification applies. Keeping
    # them apart also keeps zenohd's features from ever reaching the library
    # build, which deliberately sets ZENOHC_BUILD_WITH_UNSTABLE_API OFF.
    set(zenohd_target_dir ${CMAKE_BINARY_DIR}/zenohd-target)

    # WHERE CARGO ACTUALLY PUTS IT depends on whether it was told a target
    # triple. Given one, cargo writes <target-dir>/<triple>/release/; given
    # none, <target-dir>/release/. The Yocto recipe exports
    # CARGO_BUILD_TARGET=${RUST_TARGET_SYS} -- deliberately through the
    # environment rather than a bare --target, because that is what zenoh-c
    # already reads -- so the cross build takes the first form and a desktop
    # build the second.
    #
    # Hardcoding the desktop layout is exactly the bug this replaces: the image
    # built zenohd fine and then failed do_install with
    #
    #   file INSTALL cannot find ".../zenohd-target/release/zenohd"
    #
    # while the binary sat in .../zenohd-target/x86_64-redline-linux-gnu/release/.
    # It could only ever fail in the cross build, which is the one place nobody
    # runs by hand.
    if(NOT "$ENV{CARGO_BUILD_TARGET}" STREQUAL "")
        set(zenohd_binary ${zenohd_target_dir}/$ENV{CARGO_BUILD_TARGET}/release/zenohd)
        message(STATUS "zenohd: cross build, expecting $ENV{CARGO_BUILD_TARGET}/release/zenohd")
    else()
        set(zenohd_binary ${zenohd_target_dir}/release/zenohd)
    endif()

    # ALWAYS RUN, AND LET CARGO DECIDE. An OUTPUT rule with no DEPENDS ran
    # once and never again, so a zenoh bump or a patch change left the old
    # router installed beside a new library. Listing the inputs instead would
    # mean listing a 300-crate workspace; cargo already fingerprints it, so an
    # up-to-date build is only that check. The workspace members are path sources,
    # so cargo sees edits to them -- the git-source trap in zenoh-c.cmake does
    # not apply here.
    #
    # ALL, because redline_install_program() installs a path rather than a
    # target: nothing else would cause it to be built, and the install would
    # then land on a file that was never written. BYPRODUCTS so Ninja knows
    # where the file comes from.
    add_custom_target(zenohd ALL
        COMMAND ${REDLINE_CARGO} build --release
                -p zenohd
                --manifest-path ${zenoh_rust_SOURCE_DIR}/Cargo.toml
                --target-dir ${zenohd_target_dir}
        BYPRODUCTS ${zenohd_binary}
        COMMENT "Building zenohd from the patched zenoh workspace (cargo decides what is stale)"
        VERBATIM
    )

    redline_install_program(PROGRAM ${zenohd_binary} COMPONENT nodes)

    file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/licenses/zenohd)
    file(WRITE ${CMAKE_BINARY_DIR}/licenses/zenohd/fetch_info.txt
"Library: zenohd
Repository: https://github.com/eclipse-zenoh/zenoh.git (workspace member `zenohd`)
Tag/Version: release/1.10.0
Shallow Clone: TRUE
Patches Applied: patches/zenoh_abortable_gossip_connect.patch, inherited -- this
                 binary is built FROM the patched workspace, not against it
")
endif()
