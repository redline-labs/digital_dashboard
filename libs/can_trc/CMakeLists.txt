# PCAN .trc traces, read and written.
#
# Sits above `can` rather than beside it: the reader needs can::dlc_to_length to
# turn a CAN FD length code into a byte count, and the replay backend implements
# can::Backend. It deliberately does not depend on can_backends -- the registry
# is what pulls the backends together, and a dependency the other way would be a
# cycle.
add_library(can_trc
    src/trc.cpp
    src/trc_backend.cpp
)

target_include_directories(can_trc
    PUBLIC
        include
)

target_link_libraries(can_trc
    PUBLIC
        can
        helpers
    PRIVATE
        spdlog::spdlog
)

# Reading. The fixtures are the specification's own example blocks, one per
# version, plus the malformed input that the previous implementation turned into
# a whole-file failure.
add_executable(can_trc_test_read
    tests/test_read.cpp
)

target_link_libraries(can_trc_test_read
    PRIVATE
        can_trc
        spdlog::spdlog
)

add_project_test(TARGET can_trc_test_read LABELS can_trc unit)

# Writing, and reading back what was written. The round trip is checked against
# the two real traces in mock_data/, which is the assertion that would have
# caught the old parser returning nothing for a file with one bad line.
add_executable(can_trc_test_write
    tests/test_write.cpp
)

target_link_libraries(can_trc_test_write
    PRIVATE
        can_trc
        spdlog::spdlog
)

target_compile_definitions(can_trc_test_write
    PRIVATE
        CAN_TRC_MOCK_DATA_DIR="${CMAKE_SOURCE_DIR}/mock_data/data"
)

add_project_test(TARGET can_trc_test_write LABELS can_trc unit)

# The replay backend: pacing, bus filtering, and that stop() does not have to
# wait out the gap to the next recorded frame.
add_executable(can_trc_test_backend
    tests/test_backend.cpp
)

target_link_libraries(can_trc_test_backend
    PRIVATE
        can_trc
        spdlog::spdlog
)

add_project_test(TARGET can_trc_test_backend LABELS can_trc unit)
