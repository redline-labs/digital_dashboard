---
title: reflection
parent: Libraries
---

# reflection

## Overview

Compile-time reflection for structs and enums, in one header. `REFLECT_STRUCT`
declares a struct whose fields can be visited by name, type and value, each
carrying an optional label and description; `REFLECT_ENUM` declares an
`enum class` with constant name and value arrays and string conversion both
ways. Every configuration in the tree is declared this way, so the YAML codec,
the JSON codec, the editor's properties panel and the agent interface all walk
one field list: add a field once and all of them follow.

It is header-only and depends on nothing but the standard library. It does not
serialise anything; that is [config_codec](config_codec.html), which is generic
over this. The split keeps this header free of yaml-cpp and nlohmann, so a
struct can be reflected in a library that has no business linking either.

## Public headers

| Header | |
| --- | --- |
| `reflection/reflection.h` | `REFLECT_STRUCT`, `REFLECT_ENUM`, `visit_fields`, `enum_traits`, `enum_to_string`, `get_friendly_name`, `get_description`, and the `is_reflected_struct` / `is_reflected_enum` traits. |

## Using it

Link the CMake target `reflection`. A field is a tuple of type, name and
default, followed by an optional label and an optional description; the three
forms coexist in one struct. This is `bag_part_t` from `libs/bag`, shortened:

```cpp
REFLECT_ENUM(display_role_t, primary, secondary)

REFLECT_STRUCT(bag_part_t,
    (std::string, path, "",
        "Path", "File name, relative to the bag directory"),
    (std::uint64_t, bytes, 0,
        "Bytes", "Size on disk"),
    (bool, complete, true)
)

reflection::visit_fields(part, [](std::string_view name, auto& value, std::string_view type) {
    // every field, in declaration order
});
```

## Behaviour worth knowing

A field with no label falls back to its own name, and `FieldMetadata::annotated`
records which case applies, so a check that every field was labelled can tell
"has an entry" from "was given a label". `get_friendly_name<T>("x")` returns
the field name when nothing better exists.

Defaults are applied with parentheses, so a default of `{}` becomes
`field({})` rather than {% raw %}`field{{}}`{% endraw %}, which for a container would construct one
default element instead of none.

{: .warning }
A type containing a comma, such as `std::map<K, V>`, is split by the
preprocessor and breaks `REFLECT_STRUCT` at the member declaration. Alias it
first with `using kv_t = std::map<K, V>;`. No such field exists in the tree.

The macro tables go up to 96 fields per struct.

For enums, prefer `enum_traits<E>::try_from_string()` for anything that comes
from outside the program: a config file, a drop payload, an agent request. It
returns `std::nullopt`; `from_string()` throws `std::invalid_argument`, which
is a landmine inside a Qt event handler. `known_values()` lists the valid
spellings for an error message and caps the list at 12 entries, because the
schema registry enum has some 70.

`REFLECT_ENUM` emits `enum_names()` and `enum_values()` as free functions in
whichever namespace it is invoked in, and `is_reflected_enum` finds them by
ADL. Invoke the macro in the enum's own namespace.

Do not put a `uint8_t` in a reflected struct that will be serialised; see
[config_codec](config_codec.html) for why.

## Tests

`test_reflection` is built by the library's CMakeLists but is not registered
with `add_project_test` or `add_test`, so `ctest` does not run it. It is a demo
that prints field names, labels, descriptions and enum names for a struct using
all three field forms. The behaviour this library exists for is covered
downstream by `dashboard_widgets_test_config_roundtrip` and the other
config_codec users.
