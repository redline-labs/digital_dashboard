# Get the schema structs from the schemas target property
get_target_property(SCHEMA_STRUCTS_FROM_TARGET schemas SCHEMA_STRUCTS)

# Generate schema_registry.cpp from schema_registry.cpp.in.
# Uses the SCHEMA_CONDITIONAL_LOGIC macro to generate the conditional logic for the switch statement.
set(SCHEMA_CONDITIONAL_LOGIC "")
foreach(STRUCT_NAME ${SCHEMA_STRUCTS_FROM_TARGET})
    string(APPEND SCHEMA_CONDITIONAL_LOGIC "    if (schema_name == \"${STRUCT_NAME}\") { return capnp::Schema::from<${STRUCT_NAME}>(); }\n")
endforeach()

# Count the number of schemas.
list(LENGTH SCHEMA_STRUCTS_FROM_TARGET SCHEMA_STATIC_ARRAY_SIZE)
set(SCHEMA_STATIC_ARRAY)

# Generate a static array of schema names.
foreach(STRUCT_NAME ${SCHEMA_STRUCTS_FROM_TARGET})
    string(APPEND SCHEMA_STATIC_ARRAY "    \"${STRUCT_NAME}\",\n")
endforeach()


configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/schema_registry.cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/schema_registry.cpp"
    @ONLY
)

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/include/expression_parser/schema_registry.h"
    "${CMAKE_CURRENT_BINARY_DIR}/include/expression_parser/schema_registry.h"
    @ONLY
)

add_library(schema_registry STATIC
    ${CMAKE_CURRENT_BINARY_DIR}/schema_registry.cpp
    #capnp_data_extractor.cpp
)

target_include_directories(schema_registry PUBLIC
    "${CMAKE_CURRENT_BINARY_DIR}/include"
)

target_link_libraries(schema_registry PUBLIC
    capnp
    schemas
    common_logging
)