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
