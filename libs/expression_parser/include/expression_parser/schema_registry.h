#ifndef SCHEMA_REGISTRY_H
#define SCHEMA_REGISTRY_H

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include <capnp/schema.h>

typedef std::array<std::string_view, @SCHEMA_STATIC_ARRAY_SIZE@> available_schemas_t;

capnp::Schema get_schema_by_name(const std::string& schema_name);

constexpr available_schemas_t get_available_schemas();

#endif // SCHEMA_REGISTRY_H 
