#!/usr/bin/env bash
# Build the web console's wasm module.
#
# Lives in tools/ rather than a new top-level scripts/ because tools/ is already
# where this tree keeps developer tooling that is not part of the image
# (mcp_dashboard is a Python project sitting right here). tools/CMakeLists.txt
# adds only map_build, so a shell script in this directory is inert to the build.
#
# THE ONE THING THIS SCRIPT EXISTS FOR: capnp, capnpc-c++ and capnpc_schema_registry
# have to run on the BUILD HOST, and an Emscripten configure would produce wasm
# copies that cannot execute. cmake/NativeCodegen.cmake already solves this for
# Yocto via REDLINE_NATIVE_CODEGEN_DIR -- but it does a find_program() with
# NO_DEFAULT_PATH against ONE directory and FATAL_ERRORs if any of the three is
# missing. In a desktop build they live in two places:
#
#     build/_deps/capnproto-build/c++/src/capnp/{capnp,capnpc-c++}
#     build/schemas/capnpc_schema_registry
#
# so we stage symlinks into one directory. (Yocto gets this for free: all three
# are installed together into ${STAGING_BINDIR_NATIVE}.)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NATIVE_BUILD="${REDLINE_NATIVE_BUILD:-${ROOT}/build}"
WASM_BUILD="${REDLINE_WASM_BUILD:-${ROOT}/build-wasm}"
STAGE="${WASM_BUILD}/native-codegen"

die() { echo "build-wasm: $*" >&2; exit 1; }

command -v emcmake >/dev/null 2>&1 || die \
    "emcmake not on PATH. Source the emsdk environment first:
    source ~/emsdk/emsdk_env.sh"

[ -f "${NATIVE_BUILD}/CMakeCache.txt" ] || die \
    "no native build at ${NATIVE_BUILD}. The wasm build needs host copies of the
    code generators. Configure and build the tree normally first, or point
    REDLINE_NATIVE_BUILD at an existing build directory."

# Build the generators if they are not there yet. Cheap when they already are.
cmake --build "${NATIVE_BUILD}" --target capnp_tool capnpc_cpp capnpc_schema_registry -j"$(nproc)"

mkdir -p "${STAGE}"
stage_tool() {
    local name="$1" found
    found="$(find "${NATIVE_BUILD}" -name "${name}" -type f -perm -u+x -print -quit 2>/dev/null || true)"
    [ -n "${found}" ] || die "could not find host '${name}' under ${NATIVE_BUILD}"
    ln -sf "${found}" "${STAGE}/${name}"
    echo "  ${name} -> ${found}"
}
echo "staging host code generators in ${STAGE}:"
stage_tool capnp
stage_tool capnpc-c++
stage_tool capnpc_schema_registry

emcmake cmake -S "${ROOT}/wasm" -B "${WASM_BUILD}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DREDLINE_NATIVE_CODEGEN_DIR="${STAGE}" \
    "$@"

cmake --build "${WASM_BUILD}" -j"$(nproc)"

# Put the artifacts where the console serves them from. core::paths::resource()
# finds web/ next to the executable once installed and in the checkout
# otherwise, and cmake/ProjectInstall.cmake's redline_install_data ships the
# directory -- so this is the one place they have to land.
#
# They are BUILD OUTPUT and are gitignored: this tree commits nothing generated,
# and a committed multi-megabyte binary that drifts from the schemas it was
# built against is exactly the failure the layout fingerprint exists to catch.
ASSETS="${ROOT}/web"
mkdir -p "${ASSETS}"
cp "${WASM_BUILD}/redline.js" "${WASM_BUILD}/redline.wasm" "${ASSETS}/"
echo "installed into ${ASSETS}: redline.js redline.wasm"

echo
echo "built:"
ls -lh "${WASM_BUILD}"/redline.js "${WASM_BUILD}"/redline.wasm 2>/dev/null || true
if [ -f "${WASM_BUILD}/redline.wasm" ]; then
    echo "gzipped .wasm: $(gzip -c "${WASM_BUILD}/redline.wasm" | wc -c) bytes"
fi
