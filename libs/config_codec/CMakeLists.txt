# SPDX-License-Identifier: GPL-3.0-or-later

# Serialization, validation and clamping for configuration built out of
# REFLECT_STRUCT types: YAML both ways, JSON both ways plus a self-description
# for tooling, a range-clamping helper and a validator that reports problems by
# field path.
#
# All of this used to live in dashboard/include/dashboard/. None of it was ever
# about a dashboard -- it is generic over libs/reflection -- and the scope app
# needs the same behaviour for its workspaces and panel configs. Leaving it
# where it was would have meant a second top-level app reaching into the first
# app's include tree.
#
# Header-only, and no Qt: the JSON and YAML codecs are what the headless config
# tests exercise, and pulling Qt in here would put it in their link line for
# nothing.
add_library(config_codec INTERFACE)

target_include_directories(config_codec INTERFACE
    include
)

target_link_libraries(config_codec INTERFACE
    reflection
    helpers
    yaml-cpp
    nlohmann_json::nlohmann_json
)

# Nested validate() hooks run once, children first, each note with its path.
# String leaves (colour, keys) obey their rules in every codec. A config file is
# replaced whole or not at all.
foreach(codec_test limits string_leaf config_file)
    add_executable(config_codec_test_${codec_test} tests/test_${codec_test}.cpp)
    target_link_libraries(config_codec_test_${codec_test} PRIVATE config_codec)
    add_project_test(TARGET config_codec_test_${codec_test} LABELS config_codec unit)
endforeach()
