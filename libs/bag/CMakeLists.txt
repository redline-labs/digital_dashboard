# Recording and replaying the bus, as MCAP.
#
# A LIBRARY rather than code inside nodes/bag, for two reasons. The tests below
# are pure file I/O with no zenoh session anywhere, which is only possible if
# reading and writing are separable from the tool that drives them -- and every
# one of them exercises a failure that is silent in production (a torn part, a
# lost message, a seek that scans). And scope's DataSource seam
# (scope/include/scope/data_source.h) was designed for a recorded source; when
# that lands it links this rather than shelling out to a binary.
add_library(bag STATIC
    metadata.cpp
    writer.cpp
    reader.cpp

    # An MCAP structural validator written from the spec, using no mcap code.
    # It exists so this tree can check its own output without depending on
    # Foxglove's Go CLI being installed -- see the header.
    validate.cpp

    # Rebuilding an index from the parts, and the playback key rules. Both were
    # inside nodes/bag verbs, where neither could be tested -- and both fail
    # silently when wrong.
    rebuild.cpp
    playback.cpp

    # mcap is header-only; this is the single TU that defines
    # MCAP_IMPLEMENTATION. See the file.
    mcap_impl.cpp
)

target_include_directories(bag PUBLIC
    include
)

target_link_libraries(bag
    PUBLIC
        mcap
        # bag_metadata_t is a REFLECT_STRUCT, so consumers see reflection.
        reflection
    PRIVATE
        spdlog::spdlog
        config_codec
        # validate.cpp decompresses chunks itself rather than going through
        # mcap's reader, so it links the codecs directly.
        zstd::libzstd
        lz4::lz4
        # For pub_sub::schema_descriptor(): the schema as data, embedded in each
        # recording so a foreign consumer can decode it.
        schemas
)

# ---------------------------------------------------------------------- tests
#
# All `unit`: nothing here opens a zenoh session. That separation is deliberate
# -- see the library comment above.

# Round trip, splitting, and seeking. The three things the format is chosen for.
add_executable(bag_test_roundtrip
    test_roundtrip.cpp
)
target_link_libraries(bag_test_roundtrip PRIVATE
    bag
    spdlog::spdlog
)
add_project_test(TARGET bag_test_roundtrip LABELS bag unit)

# What happens to a recording whose writer was killed. The split-directory
# layout makes a torn last part the EXPECTED outcome of a crash, so the reader's
# behaviour there is a first-class case rather than an edge one.
add_executable(bag_test_damage
    test_damage.cpp
)
target_link_libraries(bag_test_damage PRIVATE
    bag
    spdlog::spdlog
)
add_project_test(TARGET bag_test_damage LABELS bag unit)

# The index: YAML round trip, and what a malformed or newer one does.
add_executable(bag_test_metadata
    test_metadata.cpp
)
target_link_libraries(bag_test_metadata PRIVATE
    bag
    spdlog::spdlog
)
add_project_test(TARGET bag_test_metadata LABELS bag unit)

# The bounded queue between the zenoh callbacks and the writer thread. Pure
# concurrency logic, and the place a recorder decides what to lose when it
# cannot keep up.
add_executable(bag_test_queue
    test_queue.cpp
)
target_link_libraries(bag_test_queue PRIVATE
    bag
    spdlog::spdlog
)
add_project_test(TARGET bag_test_queue LABELS bag unit)

# That what we write is structurally valid MCAP, checked against the spec rather
# than through our own reader -- which is lenient enough to have round-tripped a
# malformed file cleanly. Also the only coverage lz4 has.
add_executable(bag_test_format
    test_format.cpp
)
target_link_libraries(bag_test_format PRIVATE
    bag
    spdlog::spdlog
)
add_project_test(TARGET bag_test_format LABELS bag unit)

# That a recorded message is DECODABLE from the recording alone -- the claim the
# format was chosen for. Goes through capnp::SchemaLoader on the descriptor
# stored in the file, deliberately not through the generated headers, which
# would pass even if the recording carried no schema at all.
add_executable(bag_test_decode
    test_decode.cpp
)
target_link_libraries(bag_test_decode PRIVATE
    bag
    schemas
    capnp
    kj
    spdlog::spdlog
)
add_project_test(TARGET bag_test_decode LABELS bag unit)

# Rebuilding an index from the parts -- the recovery path for a recording whose
# recorder was killed, which decides whether a crashed run is readable at all.
add_executable(bag_test_rebuild
    test_rebuild.cpp
)
target_link_libraries(bag_test_rebuild PRIVATE
    bag
    spdlog::spdlog
)
add_project_test(TARGET bag_test_rebuild LABELS bag unit)

# The playback key rules. Pure logic, and every way of getting it wrong silently
# drops messages from a replay.
add_executable(bag_test_playback
    test_playback.cpp
)
target_link_libraries(bag_test_playback PRIVATE
    bag
    zenoh_pub_sub
    spdlog::spdlog
)
add_project_test(TARGET bag_test_playback LABELS bag unit)

# The boundaries: empty and oversized payloads, many topics, duration-based
# rolling, write-after-close, an unwritable directory, backwards ranges, and
# reader reuse. Reachable from `bag record` with the wrong arguments or a full
# disk, and most of them fail quietly if unhandled.
add_executable(bag_test_edges
    test_edges.cpp
)
target_link_libraries(bag_test_edges PRIVATE
    bag
    spdlog::spdlog
)
add_project_test(TARGET bag_test_edges LABELS bag unit)
