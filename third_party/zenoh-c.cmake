# THE RUST `zenoh` CRATE IS NOT FETCHED BY CMAKE, and that is the whole
# awkwardness of what follows. zenoh-c is a CMake project, but the library that
# actually contains the bug is its cargo dependency:
#
#     zenoh-c 1.9.0  ->  Cargo.toml: zenoh = { git = ..., branch = "release/1.9.0" }
#
# so there is no FetchContent_Declare to hang a PATCH_COMMAND off. We fetch that
# crate ourselves, patch it, and point cargo at the result with a path override.
#
# WHY BOTHER: two peer sessions that discover each other over multicast and then
# open/close repeatedly deadlock forever inside session close. It is not
# theoretical -- it made scope_test_panels time out at random under `ctest -j8`,
# and it reproduces in a 20-line program that uses zenoh and nothing of ours.
# The gossip autoconnect task is spawned UN-ABORTABLE, so the runtime's
# terminate_all_async() -- which has no timeout -- waits for a peer connection
# that will never complete. See patches/zenoh_abortable_gossip_connect.patch,
# which also records why eclipse-zenoh/zenoh#2637, the obvious candidate, does
# NOT fix it. There is no option to turn this off: a build without it deadlocks.
#
# THIS IS A BANDAID WITH AN END DATE. The same one-word change, plus a
# regression test and the reproducer, is submitted upstream from
# github.com/ryandavid/zenoh, branch fix/abortable-gossip-autoconnect. Once it
# lands in a zenoh-c release we take: delete the patch, delete this block, bump
# the tag. Everything here exists only because the fix is not upstream yet.
#
# WHY NOT LET CARGO DO THE FETCHING -- `cargo fetch` after zenoh-c is populated,
# then patch the checkout it downloaded, which needs no second clone, no pinned
# revision and no cargo flags? It was built and measured, and it does not work:
#
#     patch the checkout, ordinary build        deadlock reproduced 10 of 10
#     same patch, after deleting fingerprints   6 of 6 passed
#
# Cargo treats a git dependency's source as IMMUTABLE. It fingerprints the
# package by identity and revision rather than by file mtime, so editing a
# checkout it has already compiled changes nothing -- the build silently links
# the cached, unpatched artifact. It appears to work exactly once, on a machine
# that has never built this revision, and never again. That is the worst
# possible failure mode for a patch whose entire job is to prevent a hang.
#
# A `paths` override is the supported way round it precisely because cargo
# treats a path source as mutable: edits are noticed and rebuilds happen. The
# cost is one extra clone of a repo cargo also clones, which is 22 MB.
set(zenoh_rust_expected_sha 81c6c933b6e41d72a05f04c4442ef57717ddc72b)

# SHALLOW, AT THE BRANCH, THEN VERIFIED AGAINST THE SHA. Cloning the branch
# shallow is far smaller than a full clone at a bare commit (GIT_SHALLOW cannot
# fetch a commit, only a ref), and the check below turns the one risk that buys
# -- upstream pushing to a release branch -- into a loud failure rather than a
# silently different compile.
FetchContent_Declare(
    zenoh_rust
    GIT_REPOSITORY https://github.com/eclipse-zenoh/zenoh.git
    GIT_TAG release/1.9.0
    GIT_SHALLOW TRUE
    PATCH_COMMAND git apply ${CMAKE_SOURCE_DIR}/patches/zenoh_abortable_gossip_connect.patch
)
FetchContent_MakeAvailable(zenoh_rust)

execute_process(
    COMMAND git -C ${zenoh_rust_SOURCE_DIR} rev-parse HEAD
    OUTPUT_VARIABLE zenoh_rust_actual_sha
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT zenoh_rust_actual_sha STREQUAL zenoh_rust_expected_sha)
    message(FATAL_ERROR
            "zenoh: release/1.9.0 is now ${zenoh_rust_actual_sha}, not the "
            "${zenoh_rust_expected_sha} that zenoh-c 1.9.0's lockfile pins. Cargo would "
            "still compile the pinned revision while we patched a different one. Check "
            "what moved, re-pin zenoh_rust_expected_sha, and re-check the patch applies.")
endif()

# DID THE PATCH ACTUALLY APPLY? A failing PATCH_COMMAND does not reliably stop
# the configure -- FetchContent was seen reporting `git apply` erroring and
# carrying on -- and the consequence of not noticing is the worst kind:
# everything builds, everything passes, and the deadlock is still in there
# waiting for two processes to shut down at once. So this checks the source
# rather than trusting an exit code.
file(READ ${zenoh_rust_SOURCE_DIR}/zenoh/src/net/protocol/gossip.rs zenoh_gossip_rs)
string(FIND "${zenoh_gossip_rs}" "spawn_abortable(async move" zenoh_patch_found)
if(zenoh_patch_found EQUAL -1)
    message(FATAL_ERROR
            "zenoh: patches/zenoh_abortable_gossip_connect.patch did not apply to "
            "${zenoh_rust_SOURCE_DIR}. Building on would silently reintroduce the "
            "session-close deadlock. Delete build/_deps/zenoh_rust-* and reconfigure; "
            "if it still fails, upstream has moved and the patch needs rebasing.")
endif()

# A CARGO PATH OVERRIDE, which needs exactly one entry despite the workspace
# having thirty crates: cargo resolves `zenoh` to this copy everywhere it
# appears, including under zenoh-ext, so the whole subtree unifies on it rather
# than splitting into two versions of the same types.
#
# `paths` rather than `[patch]` because it needs no edit to zenoh-c's manifest,
# and it is precisely what the mechanism is for: same package, same version,
# patched source. It refuses to work if the patch changes DEPENDENCIES, which is
# a useful guard rail today -- ours changes only code -- but is also the thing
# to remember if this patch ever grows: a rebase that adds a crate needs
# `[patch]` instead.
#
# Written into a config file rather than passed as `--config KEY=VALUE` because
# the value is a TOML array holding an absolute path, and quoting that through
# CMake, the generator and the shell is a losing game.
set(zenoh_cargo_config ${CMAKE_BINARY_DIR}/zenoh-patch-cargo-config.toml)
file(WRITE ${zenoh_cargo_config} "paths = [\"${zenoh_rust_SOURCE_DIR}/zenoh\"]\n")

# THE DEPLOYMENT TARGET HAS TO REACH CARGO, and this config file is the only
# place it can be said from here. zenoh-c runs cargo from an add_custom_command
# (its CMakeLists.txt, `cmake -E env OPAQUE_TYPES_BUILD_DIR=... cargo build`),
# which is a *build* step: it inherits the environment of make or ninja, not the
# one CMake had while configuring. A set(ENV{MACOSX_DEPLOYMENT_TARGET}) up here
# -- which is what this file used to do -- therefore reaches nothing. It was
# measured: with CMAKE_OSX_DEPLOYMENT_TARGET pinned to 15.0, cargo still emitted
# objects built for 26.5.
#
# Without it the archive carries three different targets at once. rustc defaults
# to 11.0 for aarch64-apple-darwin, the `cc` crate compiling ring's and aws-lc's
# C sources defaults to the host's full version, and our C++ sits whereever
# CMAKE_OSX_DEPLOYMENT_TARGET puts it. Whenever the C objects come out highest,
# every link that touches zenoh reports it -- 700 "object file was built for
# newer macOS version" warnings on a 26.5 host.
#
# Cargo's [env] table applies to build scripts and rustc alike, so one entry
# covers both halves. force, because the invariant wanted here is that everything
# in the link agrees; a stale MACOSX_DEPLOYMENT_TARGET left in the developer's
# shell would otherwise win for cargo while our C++ used the CMake value, which
# is precisely the split this exists to close.
#
# The value to match is whatever the C++ side ends up at, so ask the compiler
# rather than recompute it. CMAKE_OSX_DEPLOYMENT_TARGET wins when it is set, and
# when it is not, clang has already picked something -- the host version
# truncated to major.0, currently, though that rule has changed before and is not
# ours to depend on. __ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__ reports the
# answer either way, as MMmmpp: 260000 for 26.0, 150300 for 15.3.
#
# Appended rather than written above, so `paths` stays at TOML top level: keys
# after a [table] header belong to that table.
if(APPLE)
    set(zenoh_deployment_target "${CMAKE_OSX_DEPLOYMENT_TARGET}")

    if(NOT zenoh_deployment_target)
        execute_process(
            COMMAND ${CMAKE_CXX_COMPILER} -dM -E -x c++ /dev/null
            OUTPUT_VARIABLE zenoh_compiler_defines
            ERROR_QUIET
        )
        if(zenoh_compiler_defines MATCHES
                "__ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__ ([0-9]+)")
            math(EXPR zenoh_dt_major "${CMAKE_MATCH_1} / 10000")
            math(EXPR zenoh_dt_minor "(${CMAKE_MATCH_1} / 100) % 100")
            set(zenoh_deployment_target "${zenoh_dt_major}.${zenoh_dt_minor}")
        endif()
    endif()

    if(zenoh_deployment_target)
        file(APPEND ${zenoh_cargo_config}
            "\n[env]\n"
            "MACOSX_DEPLOYMENT_TARGET = { value = \"${zenoh_deployment_target}\", force = true }\n")
        message(STATUS "zenoh: cargo deployment target ${zenoh_deployment_target}")
    else()
        # Not fatal, but say so: the build still works, it just links a mix of
        # deployment targets and reports every one of them.
        message(WARNING
            "Could not determine the macOS deployment target to hand cargo. "
            "Expect \"object file was built for newer macOS version\" warnings "
            "from libzenohc.a; set CMAKE_OSX_DEPLOYMENT_TARGET to silence them.")
    endif()
endif()

# GUARDED, because this is a cache variable being appended to: without the
# check, every reconfigure adds another `--config <same file>` and cargo rejects
# the third one with "config `include` cycle detected" -- a failure that shows up
# on the second or third build, never the first, and points at a file that is
# perfectly fine.
if(NOT "${ZENOHC_CARGO_FLAGS}" MATCHES "zenoh-patch-cargo-config")
    list(APPEND ZENOHC_CARGO_FLAGS --config ${zenoh_cargo_config})
    set(ZENOHC_CARGO_FLAGS "${ZENOHC_CARGO_FLAGS}" CACHE STRING "" FORCE)
endif()

message(STATUS "zenoh: patched for the gossip close-deadlock (${zenoh_rust_SOURCE_DIR})")

# Fetch zenoh-c
FetchContent_Declare(
    zenohc
    GIT_REPOSITORY https://github.com/eclipse-zenoh/zenoh-c.git
    GIT_TAG 1.9.0
    GIT_SHALLOW TRUE
    OVERRIDE_FIND_PACKAGE
)

# Configure zenoh-c options
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(ZENOHC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZENOHC_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZENOHC_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(zenohc)

# Copy zenoh-c license
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/licenses/zenohc)
file(COPY ${zenohc_SOURCE_DIR}/LICENSE ${zenohc_SOURCE_DIR}/README.md
     DESTINATION ${CMAKE_BINARY_DIR}/licenses/zenohc)

# Write version info
file(WRITE ${CMAKE_BINARY_DIR}/licenses/zenohc/fetch_info.txt
"Library: zenoh-c
Repository: https://github.com/eclipse-zenoh/zenoh-c.git
Tag/Version: 1.9.0
Shallow Clone: TRUE
Patches Applied: patches/zenoh_abortable_gossip_connect.patch, applied to the Rust
                 `zenoh` crate (${zenoh_rust_expected_sha}) and used via a cargo
                 path override
")
