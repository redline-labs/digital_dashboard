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
