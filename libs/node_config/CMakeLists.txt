# SPDX-License-Identifier: GPL-3.0-or-later
#
# The leaf readers every node's YAML parser shares: a failure-accumulating
# Context, range-checked integer reads, string/bool/number reads, and the topic
# key check. Nodes still parse their own structure by hand.
add_library(node_config STATIC reader.cpp)
target_include_directories(node_config PUBLIC include)
target_link_libraries(node_config
    PUBLIC yaml-cpp
    PRIVATE zenoh_pub_sub spdlog::spdlog
)

# Malformed input through every reader: wrong types, negative values into
# unsigned fields, out-of-range, non-finite, collections where a scalar
# belongs, bad keys.
add_executable(node_config_test_reader tests/test_reader.cpp)
target_link_libraries(node_config_test_reader PRIVATE node_config spdlog::spdlog)
add_project_test(TARGET node_config_test_reader LABELS node_config unit)
