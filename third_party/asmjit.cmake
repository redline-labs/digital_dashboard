# asmjit, pinned to a master commit by archive hash: it has had no release
# carrying the AArch64 assembler the HC08 JIT needs.
set(ASMJIT_COMMIT 0464e1c8f1f43c861a0c73d4e59437fff255cdd9)

FetchContent_Declare(
    asmjit
    URL https://github.com/asmjit/asmjit/archive/${ASMJIT_COMMIT}.tar.gz
    URL_HASH SHA256=2b6301e4179e782c676ec50a1ff4d58fac88f5e20ba630103ece5d2b6ea38243
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SYSTEM
)

set(ASMJIT_STATIC ON CACHE INTERNAL "")
set(ASMJIT_TEST OFF CACHE INTERNAL "")
set(ASMJIT_NO_FOREIGN ON CACHE INTERNAL "")
set(ASMJIT_NO_JIT ON CACHE INTERNAL "")
set(ASMJIT_NO_BUILDER ON CACHE INTERNAL "")
set(ASMJIT_NO_COMPILER ON CACHE INTERNAL "")
set(ASMJIT_NO_UJIT ON CACHE INTERNAL "")

FetchContent_MakeAvailable(asmjit)
# Its own warnings are its business; nothing of ours is compiled in that target.
target_compile_options(asmjit PRIVATE -w)

file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/licenses/asmjit)
file(COPY ${asmjit_SOURCE_DIR}/LICENSE.md DESTINATION ${CMAKE_BINARY_DIR}/licenses/asmjit)
file(WRITE ${CMAKE_BINARY_DIR}/licenses/asmjit/fetch_info.txt
"Library: asmjit
Repository: https://github.com/asmjit/asmjit
Tag/Version: ${ASMJIT_COMMIT}
Shallow Clone: N/A (archive)
Patches Applied: None
")
