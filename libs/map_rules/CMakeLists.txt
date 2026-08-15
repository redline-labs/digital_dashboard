cmake_minimum_required(VERSION 3.10)

project(map_rules)

# What a piece of OSM data IS -- once, for everyone who asks.
#
# THIS LIBRARY IS THE REASON THE EXTRACTOR IS OURS. The tiler and the graph
# builder both call classify(); neither keeps rules of its own. Two rule sets
# drift, and the symptom is a route down a road the map does not draw.
#
# Rules are C++ tables rather than a scripting language: this tree embeds no
# scripting, a Lua profile is precisely what owning the extractor removes, and a
# rule set that compiles is one -Wswitch-enum can check.
#
# No osm dependency on purpose. classify() takes a span of string pairs, so it
# is testable against a table of tags with no PBF anywhere near it -- and so a
# future consumer that gets its tags from somewhere else can use it unchanged.
add_library(map_rules STATIC
    src/classification.cpp
    src/labels.cpp
)

target_include_directories(map_rules PUBLIC include)

# The rules, and specifically the places where the drawn answer and the routable
# answer are DIFFERENT. A test that only checked they agree would pass while a
# car was being routed down a footpath.
add_executable(map_rules_test_classification
    tests/test_classification.cpp
)

target_link_libraries(map_rules_test_classification
    PRIVATE
        map_rules
        spdlog::spdlog
)

add_project_test(TARGET map_rules_test_classification LABELS map_rules unit)

# The label vocabulary: points of interest, peaks, airports, parks, house
# numbers. Its failures are the opposite of the road table's -- nothing crashes
# and nothing is wrong on screen, a whole layer is just quietly absent, or one
# shape is drawn in two layers at once. Every case in here was found by counting
# features against tilemaker on a real extract.
add_executable(map_rules_test_labels
    tests/test_labels.cpp
)

target_link_libraries(map_rules_test_labels
    PRIVATE
        map_rules
        spdlog::spdlog
)

add_project_test(TARGET map_rules_test_labels LABELS map_rules unit)
