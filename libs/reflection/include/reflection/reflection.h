#pragma once

// ============================================================================
// Reflection Library - Struct and Enum Reflection with Metadata Support
// ============================================================================
//
// This library provides compile-time reflection for structs and enums with
// optional field metadata (friendly names, etc.).
//
// Basic Usage:
// -----------
// 
// 1. Define a reflected struct:
//    REFLECT_STRUCT(MyStruct,
//        (int, id, 0),
//        (std::string, name, ""),
//        (double, value, 0.0)
//    )
//
// 2. Optionally add metadata with friendly names (and optional descriptions):
//    REFLECT_METADATA(MyStruct,
//        (id, "Unique Identifier", "A unique identifier for this item"),
//        (value, "Numeric Value", "The numeric value to display"),
//        (name, "Display Name")  // Description is optional, and you can skip fields
//    )
//    Note: You can provide metadata for a subset of fields. Omitted fields will
//          use their field name as the display name.
//
// 3. Use reflection:
//    - Visit fields: reflection::visit_fields(obj, [](auto name, auto& field, auto type){...})
//    - Get friendly name: reflection::get_friendly_name<MyStruct>("id")
//    - Get description: reflection::get_description<MyStruct>("id")
//    - Check for metadata: reflection::field_metadata_traits<MyStruct>::has_metadata
//
// 4. Define reflected enums:
//    REFLECT_ENUM(Color, Red, Green, Blue)
//    - Convert to string: reflection::enum_to_string(Color::Green)
//    - Convert from string: reflection::enum_traits<Color>::from_string("Green")
//
// ============================================================================

#include <array>
#include <cstddef>
#include <string_view>
#include <tuple>
#include <utility>
#include <type_traits>
#include <stdexcept>
#include <vector>

namespace reflection
{

// Trait to detect reflected structs (those generated via REFLECT_STRUCT)
template <typename T, typename = void>
struct is_reflected_struct : std::false_type {};

template <typename T>
struct is_reflected_struct<T, std::void_t<decltype(T::reflection_fields()), decltype(T::reflection_type_names())>> : std::true_type {};

template <typename T>
struct is_std_vector : std::false_type {};

template <typename U, typename Alloc>
struct is_std_vector<std::vector<U, Alloc>> : std::true_type { using value_type = U; };


// Field metadata storage
struct FieldMetadata {
    std::string_view field_name;
    std::string_view friendly_name;
    std::string_view description;
    
    constexpr FieldMetadata() : field_name(""), friendly_name(""), description("") {}
    constexpr explicit FieldMetadata(std::string_view field, std::string_view fn, std::string_view desc = "") 
        : field_name(field), friendly_name(fn), description(desc) {}
};

// Field metadata
template <typename Struct, typename FieldType>
struct FieldInfo {
    constexpr FieldInfo(std::string_view name, FieldType Struct::* ptr)
        : name(name), member_ptr(ptr) {}

    std::string_view name;
    FieldType Struct::* member_ptr;
};

// Helpers to iterate fields at compile-time
template <typename Struct, typename... FieldTypes>
struct TypeDescriptor {
    using tuple_type = std::tuple<FieldTypes Struct::*...>;
};

// visit over all fields using tuple
template <typename Object, typename Visitor, typename Tuple, std::size_t... I>
constexpr void visit_fields_impl(Object&& object, Visitor&& visitor, const Tuple& fields, std::index_sequence<I...>) {
    using Struct = std::remove_cvref_t<Object>;
    ( (visitor(
            std::get<I>(fields).name,
            (object.*(std::get<I>(fields).member_ptr)),
            Struct::reflection_type_names()[I]
        )), ... );
}

// Non-const ref form.
template <typename Struct, typename Visitor>
constexpr void visit_fields(Struct& object, Visitor&& visitor) {
    auto fields = Struct::reflection_fields();
    using Tuple = decltype(fields);
    constexpr std::size_t N = std::tuple_size<Tuple>::value;
    visit_fields_impl(object, std::forward<Visitor>(visitor), fields, std::make_index_sequence<N>{});
}

// Const ref form.
template <typename Struct, typename Visitor>
constexpr void visit_fields(const Struct& object, Visitor&& visitor) {
    auto fields = Struct::reflection_fields();
    using Tuple = decltype(fields);
    constexpr std::size_t N = std::tuple_size<Tuple>::value;
    visit_fields_impl(object, std::forward<Visitor>(visitor), fields, std::make_index_sequence<N>{});
}

// The main macro to define a struct and expose fields to reflection.
// Usage:
// REFLECT_STRUCT(MyStruct,
//   (int, a),
//   (double, b)
// );

#define REFLECTION_EXPAND(x) x
#define REFLECTION_GET_FIRST(a, b, ...) a
#define REFLECTION_GET_SECOND(a, b, ...) b
#define REFLECTION_GET_THIRD(a, b, c, ...) c

#define REFLECTION_FIELD_TYPE(pair) REFLECTION_EXPAND(REFLECTION_GET_FIRST pair)
#define REFLECTION_FIELD_NAME(pair) REFLECTION_EXPAND(REFLECTION_GET_SECOND pair)
#define REFLECTION_FIELD_INITVAL(triple) REFLECTION_EXPAND(REFLECTION_GET_THIRD triple)

#define REFLECTION_STRINGIZE_IMPL(x) #x
#define REFLECTION_STRINGIZE(x) REFLECTION_STRINGIZE_IMPL(x)

#define REFLECTION_DECLARE_MEMBER(pair) REFLECTION_FIELD_TYPE(pair) REFLECTION_FIELD_NAME(pair);
#define REFLECTION_DECLARE_MEMBER_WITH_STRUCT(StructName, pair) REFLECTION_DECLARE_MEMBER(pair)

// Helpers to expand lists either with commas (LIST) or with spaces/newlines (SEQ)
#define REFLECTION_APPLY_LIST_1(m, S, x1) m(S, x1)
#define REFLECTION_APPLY_LIST_2(m, S, x1, x2) m(S, x1), m(S, x2)
#define REFLECTION_APPLY_LIST_3(m, S, x1, x2, x3) m(S, x1), m(S, x2), m(S, x3)
#define REFLECTION_APPLY_LIST_4(m, S, x1, x2, x3, x4) m(S, x1), m(S, x2), m(S, x3), m(S, x4)
#define REFLECTION_APPLY_LIST_5(m, S, x1, x2, x3, x4, x5) m(S, x1), m(S, x2), m(S, x3), m(S, x4), m(S, x5)
#define REFLECTION_APPLY_LIST_6(m, S, x1, x2, x3, x4, x5, x6) m(S, x1), m(S, x2), m(S, x3), m(S, x4), m(S, x5), m(S, x6)
#define REFLECTION_APPLY_LIST_7(m, S, x1, x2, x3, x4, x5, x6, x7) m(S, x1), m(S, x2), m(S, x3), m(S, x4), m(S, x5), m(S, x6), m(S, x7)
#define REFLECTION_APPLY_LIST_8(m, S, x1, x2, x3, x4, x5, x6, x7, x8) m(S, x1), m(S, x2), m(S, x3), m(S, x4), m(S, x5), m(S, x6), m(S, x7), m(S, x8)
#define REFLECTION_APPLY_LIST_9(m, S, x1, x2, x3, x4, x5, x6, x7, x8, x9) m(S, x1), m(S, x2), m(S, x3), m(S, x4), m(S, x5), m(S, x6), m(S, x7), m(S, x8), m(S, x9)
#define REFLECTION_APPLY_LIST_10(m, S, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10) m(S, x1), m(S, x2), m(S, x3), m(S, x4), m(S, x5), m(S, x6), m(S, x7), m(S, x8), m(S, x9), m(S, x10)
#define REFLECTION_APPLY_LIST_11(m, S, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11) m(S, x1), m(S, x2), m(S, x3), m(S, x4), m(S, x5), m(S, x6), m(S, x7), m(S, x8), m(S, x9), m(S, x10), m(S, x11)
#define REFLECTION_APPLY_LIST_12(m, S, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12) m(S, x1), m(S, x2), m(S, x3), m(S, x4), m(S, x5), m(S, x6), m(S, x7), m(S, x8), m(S, x9), m(S, x10), m(S, x11), m(S, x12)
#define REFLECTION_APPLY_LIST_13(m, S, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13) m(S, x1), m(S, x2), m(S, x3), m(S, x4), m(S, x5), m(S, x6), m(S, x7), m(S, x8), m(S, x9), m(S, x10), m(S, x11), m(S, x12), m(S, x13)
#define REFLECTION_APPLY_LIST_14(m, S, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13, x14) m(S, x1), m(S, x2), m(S, x3), m(S, x4), m(S, x5), m(S, x6), m(S, x7), m(S, x8), m(S, x9), m(S, x10), m(S, x11), m(S, x12), m(S, x13), m(S, x14)
#define REFLECTION_APPLY_LIST_15(m, S, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13, x14, x15) m(S, x1), m(S, x2), m(S, x3), m(S, x4), m(S, x5), m(S, x6), m(S, x7), m(S, x8), m(S, x9), m(S, x10), m(S, x11), m(S, x12), m(S, x13), m(S, x14), m(S, x15)
#define REFLECTION_APPLY_LIST_16(m, S, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13, x14, x15, x16) m(S, x1), m(S, x2), m(S, x3), m(S, x4), m(S, x5), m(S, x6), m(S, x7), m(S, x8), m(S, x9), m(S, x10), m(S, x11), m(S, x12), m(S, x13), m(S, x14), m(S, x15), m(S, x16)
#define REFLECTION_APPLY_LIST_17(m, S, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13, x14, x15, x16, x17) m(S, x1), m(S, x2), m(S, x3), m(S, x4), m(S, x5), m(S, x6), m(S, x7), m(S, x8), m(S, x9), m(S, x10), m(S, x11), m(S, x12), m(S, x13), m(S, x14), m(S, x15), m(S, x16), m(S, x17)
#define REFLECTION_APPLY_LIST_18(m, S, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13, x14, x15, x16, x17, x18) m(S, x1), m(S, x2), m(S, x3), m(S, x4), m(S, x5), m(S, x6), m(S, x7), m(S, x8), m(S, x9), m(S, x10), m(S, x11), m(S, x12), m(S, x13), m(S, x14), m(S, x15), m(S, x16), m(S, x17), m(S, x18)
#define REFLECTION_APPLY_LIST_19(m, S, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13, x14, x15, x16, x17, x18, x19) m(S, x1), m(S, x2), m(S, x3), m(S, x4), m(S, x5), m(S, x6), m(S, x7), m(S, x8), m(S, x9), m(S, x10), m(S, x11), m(S, x12), m(S, x13), m(S, x14), m(S, x15), m(S, x16), m(S, x17), m(S, x18), m(S, x19)
#define REFLECTION_APPLY_LIST_20(m, S, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13, x14, x15, x16, x17, x18, x19, x20) m(S, x1), m(S, x2), m(S, x3), m(S, x4), m(S, x5), m(S, x6), m(S, x7), m(S, x8), m(S, x9), m(S, x10), m(S, x11), m(S, x12), m(S, x13), m(S, x14), m(S, x15), m(S, x16), m(S, x17), m(S, x18), m(S, x19), m(S, x20)

#define REFLECTION_APPLY_SEQ_1(m, S, x1) m(S, x1)
#define REFLECTION_APPLY_SEQ_2(m, S, x1, x2) m(S, x1) m(S, x2)
#define REFLECTION_APPLY_SEQ_3(m, S, x1, x2, x3) m(S, x1) m(S, x2) m(S, x3)
#define REFLECTION_APPLY_SEQ_4(m, S, x1, x2, x3, x4) m(S, x1) m(S, x2) m(S, x3) m(S, x4)
#define REFLECTION_APPLY_SEQ_5(m, S, x1, x2, x3, x4, x5) m(S, x1) m(S, x2) m(S, x3) m(S, x4) m(S, x5)
#define REFLECTION_APPLY_SEQ_6(m, S, x1, x2, x3, x4, x5, x6) m(S, x1) m(S, x2) m(S, x3) m(S, x4) m(S, x5) m(S, x6)
#define REFLECTION_APPLY_SEQ_7(m, S, x1, x2, x3, x4, x5, x6, x7) m(S, x1) m(S, x2) m(S, x3) m(S, x4) m(S, x5) m(S, x6) m(S, x7)
#define REFLECTION_APPLY_SEQ_8(m, S, x1, x2, x3, x4, x5, x6, x7, x8) m(S, x1) m(S, x2) m(S, x3) m(S, x4) m(S, x5) m(S, x6) m(S, x7) m(S, x8)
#define REFLECTION_APPLY_SEQ_9(m, S, x1, x2, x3, x4, x5, x6, x7, x8, x9) m(S, x1) m(S, x2) m(S, x3) m(S, x4) m(S, x5) m(S, x6) m(S, x7) m(S, x8) m(S, x9)
#define REFLECTION_APPLY_SEQ_10(m, S, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10) m(S, x1) m(S, x2) m(S, x3) m(S, x4) m(S, x5) m(S, x6) m(S, x7) m(S, x8) m(S, x9) m(S, x10)
#define REFLECTION_APPLY_SEQ_11(m, S, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11) m(S, x1) m(S, x2) m(S, x3) m(S, x4) m(S, x5) m(S, x6) m(S, x7) m(S, x8) m(S, x9) m(S, x10) m(S, x11)
#define REFLECTION_APPLY_SEQ_12(m, S, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12) m(S, x1) m(S, x2) m(S, x3) m(S, x4) m(S, x5) m(S, x6) m(S, x7) m(S, x8) m(S, x9) m(S, x10) m(S, x11) m(S, x12)
#define REFLECTION_APPLY_SEQ_13(m, S, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13) m(S, x1) m(S, x2) m(S, x3) m(S, x4) m(S, x5) m(S, x6) m(S, x7) m(S, x8) m(S, x9) m(S, x10) m(S, x11) m(S, x12) m(S, x13)
#define REFLECTION_APPLY_SEQ_14(m, S, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13, x14) m(S, x1) m(S, x2) m(S, x3) m(S, x4) m(S, x5) m(S, x6) m(S, x7) m(S, x8) m(S, x9) m(S, x10) m(S, x11) m(S, x12) m(S, x13) m(S, x14)
#define REFLECTION_APPLY_SEQ_15(m, S, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13, x14, x15) m(S, x1) m(S, x2) m(S, x3) m(S, x4) m(S, x5) m(S, x6) m(S, x7) m(S, x8) m(S, x9) m(S, x10) m(S, x11) m(S, x12) m(S, x13) m(S, x14) m(S, x15)
#define REFLECTION_APPLY_SEQ_16(m, S, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13, x14, x15, x16) m(S, x1) m(S, x2) m(S, x3) m(S, x4) m(S, x5) m(S, x6) m(S, x7) m(S, x8) m(S, x9) m(S, x10) m(S, x11) m(S, x12) m(S, x13) m(S, x14) m(S, x15) m(S, x16)
#define REFLECTION_APPLY_SEQ_17(m, S, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13, x14, x15, x16, x17) m(S, x1) m(S, x2) m(S, x3) m(S, x4) m(S, x5) m(S, x6) m(S, x7) m(S, x8) m(S, x9) m(S, x10) m(S, x11) m(S, x12) m(S, x13) m(S, x14) m(S, x15) m(S, x16) m(S, x17)
#define REFLECTION_APPLY_SEQ_18(m, S, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13, x14, x15, x16, x17, x18) m(S, x1) m(S, x2) m(S, x3) m(S, x4) m(S, x5) m(S, x6) m(S, x7) m(S, x8) m(S, x9) m(S, x10) m(S, x11) m(S, x12) m(S, x13) m(S, x14) m(S, x15) m(S, x16) m(S, x17) m(S, x18)
#define REFLECTION_APPLY_SEQ_19(m, S, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13, x14, x15, x16, x17, x18, x19) m(S, x1) m(S, x2) m(S, x3) m(S, x4) m(S, x5) m(S, x6) m(S, x7) m(S, x8) m(S, x9) m(S, x10) m(S, x11) m(S, x12) m(S, x13) m(S, x14) m(S, x15) m(S, x16) m(S, x17) m(S, x18) m(S, x19)
#define REFLECTION_APPLY_SEQ_20(m, S, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13, x14, x15, x16, x17, x18, x19, x20) m(S, x1) m(S, x2) m(S, x3) m(S, x4) m(S, x5) m(S, x6) m(S, x7) m(S, x8) m(S, x9) m(S, x10) m(S, x11) m(S, x12) m(S, x13) m(S, x14) m(S, x15) m(S, x16) m(S, x17) m(S, x18) m(S, x19) m(S, x20)

#define GET_MACRO_1_20(_1,_2,_3,_4,_5,_6,_7,_8,_9,_10,_11,_12,_13,_14,_15,_16,_17,_18,_19,_20,NAME,...) NAME
#define REFLECTION_FOR_EACH_LIST(macro, StructName, ...) REFLECTION_EXPAND(GET_MACRO_1_20(__VA_ARGS__, REFLECTION_APPLY_LIST_20, REFLECTION_APPLY_LIST_19, REFLECTION_APPLY_LIST_18, REFLECTION_APPLY_LIST_17, REFLECTION_APPLY_LIST_16, REFLECTION_APPLY_LIST_15, REFLECTION_APPLY_LIST_14, REFLECTION_APPLY_LIST_13, REFLECTION_APPLY_LIST_12, REFLECTION_APPLY_LIST_11, REFLECTION_APPLY_LIST_10, REFLECTION_APPLY_LIST_9, REFLECTION_APPLY_LIST_8, REFLECTION_APPLY_LIST_7, REFLECTION_APPLY_LIST_6, REFLECTION_APPLY_LIST_5, REFLECTION_APPLY_LIST_4, REFLECTION_APPLY_LIST_3, REFLECTION_APPLY_LIST_2, REFLECTION_APPLY_LIST_1)(macro, StructName, __VA_ARGS__))
#define REFLECTION_FOR_EACH_SEQ(macro, StructName, ...) REFLECTION_EXPAND(GET_MACRO_1_20(__VA_ARGS__, REFLECTION_APPLY_SEQ_20, REFLECTION_APPLY_SEQ_19, REFLECTION_APPLY_SEQ_18, REFLECTION_APPLY_SEQ_17, REFLECTION_APPLY_SEQ_16, REFLECTION_APPLY_SEQ_15, REFLECTION_APPLY_SEQ_14, REFLECTION_APPLY_SEQ_13, REFLECTION_APPLY_SEQ_12, REFLECTION_APPLY_SEQ_11, REFLECTION_APPLY_SEQ_10, REFLECTION_APPLY_SEQ_9, REFLECTION_APPLY_SEQ_8, REFLECTION_APPLY_SEQ_7, REFLECTION_APPLY_SEQ_6, REFLECTION_APPLY_SEQ_5, REFLECTION_APPLY_SEQ_4, REFLECTION_APPLY_SEQ_3, REFLECTION_APPLY_SEQ_2, REFLECTION_APPLY_SEQ_1)(macro, StructName, __VA_ARGS__))

// Initializer-supporting variant: each field must be a triple (Type, name, init)
// Use parentheses in member initializers so that `{}` becomes `field({})`.
// This avoids accidental nested-brace initialization like `field{{}}`
// which can construct a container with a single default element.
#define REFLECTION_INIT_EXPR(StructName, triple) REFLECTION_FIELD_NAME(triple)( REFLECTION_FIELD_INITVAL(triple) )
#define REFLECTION_DECLARE_MEMBER_FROM_TRIPLE(StructName, triple) REFLECTION_FIELD_TYPE(triple) REFLECTION_FIELD_NAME(triple);

#define REFLECTION_FIELD_INFO_TRIPLE(StructName, triple) \
    ::reflection::FieldInfo<StructName, REFLECTION_FIELD_TYPE(triple)>( \
        std::string_view{REFLECTION_STRINGIZE(REFLECTION_FIELD_NAME(triple))}, &StructName::REFLECTION_FIELD_NAME(triple))

// Produce a stringified type name for each field (as std::string_view)
#define REFLECTION_FIELD_TYPE_NAME(StructName, triple) \
    std::string_view{REFLECTION_STRINGIZE(REFLECTION_FIELD_TYPE(triple))}

#define REFLECT_STRUCT(StructName, ...) \
struct StructName { \
    REFLECTION_FOR_EACH_SEQ(REFLECTION_DECLARE_MEMBER_FROM_TRIPLE, StructName, __VA_ARGS__) \
    StructName() \
        : REFLECTION_FOR_EACH_LIST(REFLECTION_INIT_EXPR, StructName, __VA_ARGS__) \
    {} \
    static constexpr auto reflection_fields() { \
        using This = StructName; \
        return std::make_tuple( REFLECTION_FOR_EACH_LIST(REFLECTION_FIELD_INFO_TRIPLE, This, __VA_ARGS__) ); \
    } \
    static constexpr auto reflection_type_names() { \
        return std::array{ REFLECTION_FOR_EACH_LIST(REFLECTION_FIELD_TYPE_NAME, StructName, __VA_ARGS__) }; \
    } \
};

// =========================
// Field metadata utilities
// =========================

// Default metadata traits - can be specialized for each struct
template <typename T>
struct field_metadata_traits {
    static constexpr auto metadata() {
        // Return empty array by default (size 0)
        return std::array<FieldMetadata, 0>{};
    }
    static constexpr bool has_metadata = false;
};

// Helper implementation - search metadata by field name
template <typename Struct, typename MetadataArray, std::size_t... I>
constexpr std::string_view get_friendly_name_impl(
    const MetadataArray& metadata, 
    std::string_view field_name,
    std::index_sequence<I...>)
{
    std::string_view result = field_name;
    (void)((metadata[I].field_name == field_name && !metadata[I].friendly_name.empty() ? 
        (result = metadata[I].friendly_name, true) : false) || ...);
    return result;
}

// Helper to get friendly name for a field by name
template <typename Struct>
constexpr std::string_view get_friendly_name(std::string_view field_name)
{
    if constexpr (field_metadata_traits<Struct>::has_metadata)
    {
        constexpr auto metadata = field_metadata_traits<Struct>::metadata();
        return get_friendly_name_impl<Struct>(metadata, field_name, std::make_index_sequence<metadata.size()>{});
    }
    else
    {
        return field_name;
    }
}

// Helper implementation for description lookup - search metadata by field name
template <typename Struct, typename MetadataArray, std::size_t... I>
constexpr std::string_view get_description_impl(
    const MetadataArray& metadata, 
    std::string_view field_name,
    std::index_sequence<I...>)
{
    std::string_view result = "";
    (void)((metadata[I].field_name == field_name && !metadata[I].description.empty() ? 
        (result = metadata[I].description, true) : false) || ...);
    return result;
}

// Helper to get description for a field by name
template <typename Struct>
constexpr std::string_view get_description(std::string_view field_name)
{
    if constexpr (field_metadata_traits<Struct>::has_metadata)
    {
        constexpr auto metadata = field_metadata_traits<Struct>::metadata();
        return get_description_impl<Struct>(metadata, field_name, std::make_index_sequence<metadata.size()>{});
    }
    else
    {
        return "";
    }
}

// Macro helpers for metadata - use different extractors to avoid conflicts
#define REFLECTION_METADATA_GET_FIELD(field, friendly, ...) field
#define REFLECTION_METADATA_GET_FRIENDLY(field, friendly, ...) friendly
#define REFLECTION_METADATA_GET_DESCRIPTION(field, friendly, ...) __VA_ARGS__

#define REFLECTION_METADATA_PAIR_FIELD(pair) REFLECTION_EXPAND(REFLECTION_METADATA_GET_FIELD pair)
#define REFLECTION_METADATA_PAIR_FRIENDLY(pair) REFLECTION_EXPAND(REFLECTION_METADATA_GET_FRIENDLY pair)
#define REFLECTION_METADATA_PAIR_DESCRIPTION(pair) REFLECTION_EXPAND(REFLECTION_METADATA_GET_DESCRIPTION pair)

// Helper to create a FieldMetadata entry for a tuple (field_name, "Friendly Name") or (field_name, "Friendly Name", "Description")
// The field name is stringified and stored so we can do name-based lookups
#define REFLECTION_MAKE_METADATA_ENTRY(StructName, pair) \
    ::reflection::FieldMetadata{REFLECTION_STRINGIZE(REFLECTION_METADATA_PAIR_FIELD(pair)), REFLECTION_METADATA_PAIR_FRIENDLY(pair), REFLECTION_METADATA_PAIR_DESCRIPTION(pair)}

// Macro to define metadata for a struct by specializing field_metadata_traits
// Usage: REFLECT_METADATA(MyStruct, (field1, "Friendly Field 1"), (field2, "Friendly Field 2"))
// Note: You can provide metadata for a subset of fields - omitted fields will use their field name as the friendly name
#define REFLECT_METADATA(StructName, ...) \
namespace reflection { \
template <> \
struct field_metadata_traits<StructName> { \
    static constexpr auto metadata() { \
        return std::array{ \
            REFLECTION_FOR_EACH_LIST(REFLECTION_MAKE_METADATA_ENTRY, StructName, __VA_ARGS__) \
        }; \
    } \
    static constexpr bool has_metadata = true; \
}; \
}

// =========================
// Enum reflection utilities
// =========================

template <typename Enum>
struct enum_traits
{
    static constexpr auto names()
    {
        // ADL: finds enum_names(Enum{}) in the enum's namespace
        return enum_names(Enum{});
    }

    static constexpr auto values()
    {
        // ADL: finds enum_values(Enum{}) in the enum's namespace
        return enum_values(Enum{});
    }

    static constexpr std::string_view to_string(Enum v)
    {
        constexpr auto n = names();
        constexpr auto vs = values();
        for (std::size_t i = 0; i < n.size(); ++i)
        {
            if (vs[i] == v) return n[i];
        }
        return std::string_view{};
    }

    static constexpr Enum from_string(std::string_view s)
    {
        constexpr auto n = names();
        constexpr auto vs = values();
        for (std::size_t i = 0; i < n.size(); ++i)
        {
            if (n[i] == s) return vs[i];
        }
        throw std::invalid_argument("Invalid string for enum");
    }
};

template <typename Enum>
constexpr std::string_view enum_to_string(Enum v)
{
    return enum_traits<Enum>::to_string(v);
}


// Single-token enum item helpers (stringify identifier directly)
#define REFLECTION_ENUM_DECLARE_VALUE(EnumName, x) x
#define REFLECTION_ENUM_NAME_ITEM(EnumName, x) std::string_view{ REFLECTION_STRINGIZE(x) }
#define REFLECTION_ENUM_VALUE_ITEM(EnumName, x) EnumName::x
#define REFLECTION_ENUM_SWITCH_CASE(EnumName, x) case EnumName::x: return std::string_view{ REFLECTION_STRINGIZE(x) };

// Define a strongly-typed enum with string conversion helpers and constexpr name/value arrays.
// Usage:
// REFLECT_ENUM(MyEnum, Red, Green, Blue)
#define REFLECT_ENUM(EnumName, ...) \
enum class EnumName { \
    REFLECTION_FOR_EACH_LIST(REFLECTION_ENUM_DECLARE_VALUE, EnumName, __VA_ARGS__) \
}; \
/* Provide ADL hooks in the enum's namespace */ \
constexpr auto enum_names(EnumName) { \
    return std::array{ REFLECTION_FOR_EACH_LIST(REFLECTION_ENUM_NAME_ITEM, EnumName, __VA_ARGS__) }; \
} \
constexpr auto enum_values(EnumName) { \
    return std::array{ REFLECTION_FOR_EACH_LIST(REFLECTION_ENUM_VALUE_ITEM, EnumName, __VA_ARGS__) }; \
}

}  // namespace reflection
