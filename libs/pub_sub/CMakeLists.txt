# pub_sub/schema_registry.h and its .cpp are generated in schemas/ by
# tools/schema_registry_plugin.cpp and reach us through the `schemas` target's
# include directories. Nothing about the schema list is known here, which is the
# point: this file used to rebuild the registry out of CMake strings read back
# from target properties on `schemas`, and the two halves could disagree.

add_library(zenoh_pub_sub STATIC
    zenoh_subscriber.cpp
    session_manager.cpp

    # Decode-a-payload-and-evaluate-an-expression, with no zenoh in it. Split
    # out of zenoh_subscriber.cpp because two consumers need it without a live
    # subscription: a recorded-data source, and anything that wants to subscribe
    # once per key and evaluate several expressions per sample.
    expression_evaluator.cpp

    # The public way to get raw payloads plus their schema name, for a consumer
    # that decodes them itself.
    raw_subscriber.cpp

    # One liveliness token per process, mapping this session's zid to a name a
    # person recognises. The other half of the zid an advertisement carries, and
    # the only way a process that subscribes but never publishes is visible at
    # all.
    node_identity.cpp

    # The zenoh boundary. ZenohPublisher/ZenohTypedSubscriber are templated on a
    # capnp schema and so must be defined in headers; these hold the parts that
    # are not templated, so <zenoh.hxx> is compiled once here instead of in every
    # translation unit that touches the bus. See detail/byte_publisher.h.
    detail/byte_publisher.cpp
    detail/byte_subscriber.cpp

    # Generic capnp<->JSON over the dynamic API, and topic observation. Shared by
    # nodes/inspect and the agent control interface so the two cannot disagree
    # about what is on the bus.
    capnp_json.cpp
    topic_discovery.cpp

    # Topic key charset rules and the mangling that packs a key into a
    # liveliness token's key expression. Pure string logic, no zenoh.
    topic_key.cpp

    # Discovery via zenoh liveliness: topics appear the moment a node starts,
    # without waiting for it to publish anything.
    topic_directory.cpp
)

target_include_directories(zenoh_pub_sub PUBLIC
    include
    "${CMAKE_CURRENT_SOURCE_DIR}/../../include"  # TODO: This is a hack to get the helpers.h file included.
)

target_link_libraries(zenoh_pub_sub PUBLIC
    capnp
    schemas
    spdlog::spdlog
    exprtk::exprtk
    zenohcxx::zenohc
    helpers
    reflection
    nlohmann_json::nlohmann_json
)


# The publish-side encoding string against the subscribe-side registry lookup.
# Needs no zenoh session -- zenoh::Encoding is a standalone value type -- so it
# drives the real encoder rather than a re-implementation of it. That is the
# point: a test that built the string itself would have agreed with the bug.
add_executable(pub_sub_test_capnp_encoding
    test_capnp_encoding.cpp
)

target_link_libraries(pub_sub_test_capnp_encoding PRIVATE
    zenoh_pub_sub
    spdlog::spdlog
)
add_project_test(TARGET pub_sub_test_capnp_encoding LABELS pub_sub unit)

# NTP64 conversion. Pure arithmetic over a header, so `unit` -- and it has to be
# a test rather than a careful reading, because every way of getting the
# conversion wrong yields a plausible number instead of an error. A timestamp off
# by 2^32 looks exactly like a timestamp until someone plots it.
add_executable(pub_sub_test_timestamp
    test_timestamp.cpp
)

target_link_libraries(pub_sub_test_timestamp PRIVATE
    zenoh_pub_sub
    spdlog::spdlog
)
add_project_test(TARGET pub_sub_test_timestamp LABELS pub_sub unit)

# That timestamping is actually enabled, which only a real bus can say. The
# arithmetic above is testable in isolation; "does zenoh stamp our samples" is
# not, and it is the half that fails silently -- a peer session with the default
# config stamps nothing and reports nothing. `net`, self-skipping.
add_executable(pub_sub_test_sample_metadata
    test_sample_metadata.cpp
)

target_link_libraries(pub_sub_test_sample_metadata PRIVATE
    zenoh_pub_sub
    schemas
    spdlog::spdlog
)
add_project_test(TARGET pub_sub_test_sample_metadata LABELS pub_sub net)

# SessionManager lifecycle. Unlike the encoding test this one does open real
# zenoh sessions -- releasing one and opening another is the whole point -- so
# it skips itself (exit 0, with a warning) on a host where none can be opened.
add_executable(pub_sub_test_session_manager
    test_session_manager.cpp
)

target_link_libraries(pub_sub_test_session_manager PRIVATE
    zenoh_pub_sub
    spdlog::spdlog
)
add_project_test(TARGET pub_sub_test_session_manager LABELS pub_sub net)

# Expression evaluation, with the emphasis on payloads a publisher should never
# send. Constructing a subscriber opens a session, so this is `net` and skips
# itself where none can be opened.
#
# This replaces test_expression_parser.cpp, which was commented out of the build
# and had rotted against the API it was written for -- it constructed the
# subscriber with a schema *name*, called a public non-templated evaluate(), and
# asserted that an invalid parser throws. None of those had been true for a long
# time, so the whole decode/evaluate path had no coverage at all.
add_executable(pub_sub_test_expression_eval
    test_expression_eval.cpp
)

target_link_libraries(pub_sub_test_expression_eval PRIVATE
    zenoh_pub_sub
    spdlog::spdlog
)
add_project_test(TARGET pub_sub_test_expression_eval LABELS pub_sub net)

# The same decode-and-evaluate behaviour, but against ExpressionEvaluator
# directly. No session, so `unit` rather than `net`: this one cannot skip itself
# on a machine with no bus, which is most of the point of having split the
# evaluator out of the subscriber. The `net` test above stays as it was, because
# an unchanged test passing is the evidence the split changed nothing.
add_executable(pub_sub_test_expression_evaluator
    test_expression_evaluator.cpp
)

target_link_libraries(pub_sub_test_expression_evaluator PRIVATE
    zenoh_pub_sub
    spdlog::spdlog
)
add_project_test(TARGET pub_sub_test_expression_evaluator LABELS pub_sub unit)

# ZenohPublisher reuses one buffer as the builder's first segment, so the
# contents of consecutive messages -- especially either side of the size where
# that buffer has to grow -- are a correctness question. Drives a real publisher
# into a real subscriber, so it needs a session: `net`.
add_executable(pub_sub_test_publisher_reuse
    test_publisher_reuse.cpp
)

target_link_libraries(pub_sub_test_publisher_reuse PRIVATE
    zenoh_pub_sub
    spdlog::spdlog
)
add_project_test(TARGET pub_sub_test_publisher_reuse LABELS pub_sub net)

# Topic key validation and mangling. Pure logic, so `unit`. The round trip over
# every key in configs/ is the part that would catch a real regression: it
# asserts the assumption the advertisement scheme rests on against the actual
# corpus rather than against examples chosen to pass.
add_executable(pub_sub_test_topic_key
    test_topic_key.cpp
)

target_compile_definitions(pub_sub_test_topic_key PRIVATE
    REDLINE_CONFIG_DIR="${CMAKE_SOURCE_DIR}/configs")

target_link_libraries(pub_sub_test_topic_key PRIVATE
    zenoh_pub_sub
    spdlog::spdlog
)
add_project_test(TARGET pub_sub_test_topic_key LABELS pub_sub unit)

# Discovery via liveliness, end to end. The claim it exists to pin: a topic
# appears from the publisher alone, with no sample ever published -- which is
# exactly what traffic-observing discovery cannot do. Needs a session: `net`.
add_executable(pub_sub_test_topic_directory
    test_topic_directory.cpp
)

target_link_libraries(pub_sub_test_topic_directory PRIVATE
    zenoh_pub_sub
    spdlog::spdlog
)
add_project_test(TARGET pub_sub_test_topic_directory LABELS pub_sub net)
