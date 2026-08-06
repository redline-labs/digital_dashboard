# Fetch lz4
#
# The other compression codec MCAP supports. Not the bag default -- zstd wins on
# ratio at a speed that is still ample -- but worth having, because lz4 is the
# right answer when the recorder is CPU-bound rather than disk-bound, which is
# the case on the embedded target.
#
# It is also not optional in practice: without it, mcap has to be compiled with
# MCAP_COMPRESSION_NO_LZ4, and a reader built that way cannot open a file
# somebody else recorded with lz4. Being able to READ every codec the format
# defines matters more than which one we write.
set(LZ4_GIT_TAG v1.10.0)

FetchContent_Declare(
    lz4
    GIT_REPOSITORY https://github.com/lz4/lz4.git
    GIT_TAG ${LZ4_GIT_TAG}
    GIT_SHALLOW TRUE
    # Same situation as zstd: the CMake project is in a subdirectory.
    SOURCE_SUBDIR build/cmake
)

set(LZ4_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(LZ4_BUILD_LEGACY_LZ4C OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(lz4)

# Normalised for the same reason as zstd's: lz4's exported target name has
# changed across releases (lz4_static, then LZ4::lz4_static).
if(NOT TARGET lz4::lz4)
    if(TARGET lz4_static)
        add_library(lz4::lz4 ALIAS lz4_static)
    elseif(TARGET LZ4::lz4_static)
        add_library(lz4::lz4 ALIAS LZ4::lz4_static)
    else()
        message(FATAL_ERROR "lz4 ${LZ4_GIT_TAG} exported no static target this file knows about.")
    endif()
endif()

# No include directory is added here, for the reason spelled out in zstd.cmake:
# lz4 exports its own as a $<BUILD_INTERFACE:> generator expression, and adding
# a bare path beside it makes CMake refuse to generate.

# Copy lz4 license
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/licenses/lz4)
file(COPY ${lz4_SOURCE_DIR}/LICENSE ${lz4_SOURCE_DIR}/README.md
     DESTINATION ${CMAKE_BINARY_DIR}/licenses/lz4)

file(WRITE ${CMAKE_BINARY_DIR}/licenses/lz4/fetch_info.txt
"Library: lz4
Repository: https://github.com/lz4/lz4.git
Tag/Version: ${LZ4_GIT_TAG}
Shallow Clone: TRUE
Patches Applied: None
")
