# Fetch mcap
#
# The container format for `bag`. Chosen because it is the only widely-used
# format that already speaks our serialization: "capnproto" is a REGISTERED
# value for both schema_encoding and message_encoding in the MCAP format
# registry (https://mcap.dev/spec/registry), so a recorded payload goes in
# byte-for-byte and Foxglove Studio opens the result with no work from us.
#
# It also has the two structural properties a multi-gigabyte recording needs:
# per-chunk compression, and a summary section carrying ChunkIndex (time range
# -> file offset) plus Statistics -- so `bag info` reads counts and duration
# without scanning, and seeking is a seek rather than a skip.
set(MCAP_GIT_TAG releases/cpp/v2.1.3)

FetchContent_Declare(
    mcap
    GIT_REPOSITORY https://github.com/foxglove/mcap.git
    GIT_TAG ${MCAP_GIT_TAG}
    GIT_SHALLOW TRUE
)

# Populate WITHOUT configuring. mcap's repository is a monorepo -- Go, Python,
# Rust, TypeScript, docs and conformance suites -- and its C++ directory has no
# CMakeLists.txt at all. There is nothing to add_subdirectory(); we want the
# headers and nothing else.
FetchContent_GetProperties(mcap)
if(NOT mcap_POPULATED)
    FetchContent_Populate(mcap)
endif()

# Header-only upstream: the implementation lives in .inl files included by the
# headers, guarded by MCAP_IMPLEMENTATION so exactly one translation unit
# compiles them. libs/bag/mcap_impl.cpp is that translation unit, which keeps
# the ~10k lines of implementation out of every file that merely reads an
# mcap::Message.
#
# Deliberately NOT using olympus-robotics/mcap_builder, which mcap.dev suggests:
# it tracks GIT_TAG main, and an unpinned dependency is incompatible with how
# everything else in this directory is fetched. exprtk already taught this tree
# what an unpinned upstream costs (see exprtk.cmake).
add_library(mcap INTERFACE)
target_include_directories(mcap SYSTEM INTERFACE ${mcap_SOURCE_DIR}/cpp/mcap/include)

# Both codecs. Without these definitions mcap compiles its compression support
# out entirely, and a reader built that way cannot open a file recorded by
# anything else -- which is a far worse limitation than the two dependencies.
target_link_libraries(mcap INTERFACE zstd::libzstd lz4::lz4)

# Copy mcap license
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/licenses/mcap)
file(COPY ${mcap_SOURCE_DIR}/LICENSE DESTINATION ${CMAKE_BINARY_DIR}/licenses/mcap)

file(WRITE ${CMAKE_BINARY_DIR}/licenses/mcap/fetch_info.txt
"Library: mcap
Repository: https://github.com/foxglove/mcap.git
Tag/Version: ${MCAP_GIT_TAG}
Shallow Clone: TRUE
Patches Applied: None
Note: header-only; MCAP_IMPLEMENTATION is defined in libs/bag/mcap_impl.cpp.
")
