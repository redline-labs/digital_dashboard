# Get the schema structs from the schemas target property
get_target_property(SCHEMA_STRUCTS_FROM_TARGET schemas SCHEMA_STRUCTS)
get_target_property(SCHEMA_HEADER_FILES_FROM_TARGET schemas SCHEMA_HEADER_FILES)

# Generate schema_registry.cpp from schema_registry.cpp.in.
# Uses the SCHEMA_CONDITIONAL_LOGIC macro to generate the conditional logic for the switch statement.
set(SCHEMA_CONDITIONAL_LOGIC "")
foreach(STRUCT_NAME ${SCHEMA_STRUCTS_FROM_TARGET})
    string(APPEND SCHEMA_CONDITIONAL_LOGIC "    if (schema_name == \"${STRUCT_NAME}\") { return capnp::Schema::from<${STRUCT_NAME}>(); }\n")
endforeach()

# Generate lookup by schema_type_t.
set(SCHEMA_CONDITIONAL_LOGIC_BY_TYPE "")
foreach(STRUCT_NAME ${SCHEMA_STRUCTS_FROM_TARGET})
    string(APPEND SCHEMA_CONDITIONAL_LOGIC_BY_TYPE "    if (schema_type == schema_type_t::${STRUCT_NAME}) { return capnp::Schema::from<${STRUCT_NAME}>(); }\n")
endforeach()

set(SCHEMA_INCLUDES "")
foreach(SCHEMA_HEADER_FILE ${SCHEMA_HEADER_FILES_FROM_TARGET})
    string(APPEND SCHEMA_INCLUDES "#include \"${SCHEMA_HEADER_FILE}\"\n")
endforeach()

# Count the number of schemas.
list(LENGTH SCHEMA_STRUCTS_FROM_TARGET SCHEMA_STATIC_ARRAY_SIZE)
set(SCHEMA_STATIC_ARRAY)

# Generate a static array of schema names.
foreach(STRUCT_NAME ${SCHEMA_STRUCTS_FROM_TARGET})
    string(APPEND SCHEMA_STATIC_ARRAY "    \"${STRUCT_NAME}\",\n")
endforeach()

# Generate a enum list of schema names.
foreach(STRUCT_NAME ${SCHEMA_STRUCTS_FROM_TARGET})
    string(APPEND SCHEMA_ENUM_LIST ",\n    ${STRUCT_NAME}")
endforeach()


configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/schema_registry.cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/schema_registry.cpp"
    @ONLY
)

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/include/pub_sub/schema_registry.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/include/pub_sub/schema_registry.h"
    @ONLY
)

add_library(zenoh_pub_sub STATIC
    ${CMAKE_CURRENT_BINARY_DIR}/schema_registry.cpp
    zenoh_subscriber.cpp
    session_manager.cpp
)

target_include_directories(zenoh_pub_sub PUBLIC
    include
    "${CMAKE_CURRENT_BINARY_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../include"  # TODO: This is a hack to get the helpers.h file included.
)

target_link_libraries(zenoh_pub_sub PUBLIC
    capnp
    schemas
    common_logging
    exprtk::exprtk
    zenohcxx::zenohc
    helpers
    reflection
)


# Add test executable
#add_executable(test_expression_parser
#    test_expression_parser.cpp
#)

#target_link_libraries(test_expression_parser
#    expression_parser
#    schemas
#    capnp
#    kj
#)
