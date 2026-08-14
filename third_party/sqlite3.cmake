# Fetch SQLite
#
# Here for libs/mbtiles: an .mbtiles archive *is* an SQLite database, so reading
# one is not optional.
#
# The official amalgamation rather than the sqlite/sqlite repository, because
# that repository ships the *source* of the amalgamation and building it needs
# tclsh to run the generator first.  The zip is the artifact everyone actually
# compiles, and it is one .c file.
#
# There is no git tag to pin, so the pin is the hash sqlite.org publishes beside
# the download.  A changed archive fails the configure rather than silently
# compiling something else.
#
# SHA3-256, not SHA-256.  The hash in sqlite.org's download table is SHA3, and
# pasting it into a `SHA256=` line produces a mismatch against a file that is
# perfectly genuine -- which reads like a corrupted download or a tampered
# mirror.  CMake understands SHA3_256, so the pin is the published string
# verbatim with no conversion step to get wrong.
set(SQLITE3_VERSION 3.53.4)
set(SQLITE3_URL https://www.sqlite.org/2026/sqlite-amalgamation-3530400.zip)
set(SQLITE3_SHA3_256 628a44cfe82c66aed1ccbbe85a562d2e33ebe64b3288981ed76285612227934e)

FetchContent_Declare(
    sqlite3
    URL ${SQLITE3_URL}
    URL_HASH SHA3_256=${SQLITE3_SHA3_256}
)

# The archive contains no CMakeLists.txt, so this populates and skips configure
# -- the same "give me the sources, I will describe the target" outcome
# mcap.cmake gets from SOURCE_SUBDIR.
FetchContent_MakeAvailable(sqlite3)

add_library(sqlite3 STATIC ${sqlite3_SOURCE_DIR}/sqlite3.c)

# NOT a SYSTEM include directory, deliberately.  sqlite3.h is also a Homebrew
# formula, and Homebrew's prefix reaches this build through Qt6::Platform (see
# the comment at the top of the root CMakeLists.txt).  Both would be -isystem
# and Qt's entry is emitted first, so a SYSTEM include here would let a brewed
# sqlite3.h -- a different version, possibly built with different SQLITE_*
# options than the .c we compile -- win.  A plain include directory is searched
# ahead of every -isystem entry and cannot lose that race.
target_include_directories(sqlite3 PUBLIC ${sqlite3_SOURCE_DIR})

target_compile_definitions(sqlite3
    PUBLIC
        # Serialized, not just "safe from separate threads".  The zenoh
        # queryable in nodes/map_server is called concurrently on several RX
        # threads against one connection, and the weaker SQLITE_THREADSAFE=2
        # would make that undefined rather than merely contended.
        SQLITE_THREADSAFE=1
    PRIVATE
        # Nothing here loads extensions or wants a double-quoted string to
        # silently become a string literal when it does not name a column --
        # the misspelled-identifier bug SQLite keeps for backwards
        # compatibility.
        SQLITE_OMIT_LOAD_EXTENSION
        SQLITE_DQS=0
        SQLITE_DEFAULT_MEMSTATUS=0
        SQLITE_OMIT_DEPRECATED
)

# The amalgamation is C, and it does not build clean under this tree's warning
# set.  Those flags are ours to hold our own code to, not upstream's.
target_compile_options(sqlite3 PRIVATE -w)

add_library(sqlite3::sqlite3 ALIAS sqlite3)

# SQLite is in the public domain and ships no LICENSE file; the dedication is a
# comment at the top of sqlite3.h.  Record the fact rather than copy nothing.
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/licenses/sqlite3)
file(WRITE ${CMAKE_BINARY_DIR}/licenses/sqlite3/LICENSE
"SQLite is in the Public Domain.

The author disclaims copyright to the SQLite source code.  See
https://www.sqlite.org/copyright.html and the blessing at the top of
sqlite3.h.
")

file(WRITE ${CMAKE_BINARY_DIR}/licenses/sqlite3/fetch_info.txt
"Library: sqlite3 (amalgamation)
Repository: ${SQLITE3_URL}
Tag/Version: ${SQLITE3_VERSION}
Shallow Clone: N/A (source archive, pinned by SHA3-256 ${SQLITE3_SHA3_256})
Patches Applied: None
")
