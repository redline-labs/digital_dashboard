#include "pub_sub/capnp_json.h"

#include "pub_sub/capnp_payload.h"

#include "helpers/hex.h"

#include <capnp/serialize.h>

#include <cmath>
#include <limits>

namespace pub_sub
{

namespace
{

json dynamicToJson(capnp::DynamicValue::Reader value, const CapnpJsonOptions& options);

json listToJson(capnp::DynamicList::Reader list, const CapnpJsonOptions& options)
{
    json out = json::array();
    for (auto element : list)
    {
        out.push_back(dynamicToJson(element, options));
    }
    return out;
}

json structToJson(capnp::DynamicStruct::Reader reader, const CapnpJsonOptions& options)
{
    json out = json::object();

    // Non-union fields plus whichever union arm is active. Asking for an
    // inactive union field throws, so this is the only safe traversal.
    for (auto field : reader.getSchema().getNonUnionFields())
    {
        out[field.getProto().getName().cStr()] = dynamicToJson(reader.get(field), options);
    }

    // KJ_IF_MAYBE binds a pointer, not a reference, in this capnp version.
    KJ_IF_MAYBE(active, reader.which())
    {
        out[active->getProto().getName().cStr()] = dynamicToJson(reader.get(*active), options);
    }

    return out;
}

json dynamicToJson(capnp::DynamicValue::Reader value, const CapnpJsonOptions& options)
{
    switch (value.getType())
    {
        case capnp::DynamicValue::VOID:
            return nullptr;
        case capnp::DynamicValue::BOOL:
            return value.as<bool>();
        case capnp::DynamicValue::INT:
            return value.as<std::int64_t>();
        case capnp::DynamicValue::UINT:
            return value.as<std::uint64_t>();
        case capnp::DynamicValue::FLOAT:
            return value.as<double>();
        case capnp::DynamicValue::TEXT:
            return std::string(value.as<capnp::Text>().cStr());
        case capnp::DynamicValue::DATA:
        {
            const auto data = value.as<capnp::Data>();
            const std::span<const std::uint8_t> bytes(data.begin(), data.size());

            // Hex when the caller asked for it and the bytes fit: a service
            // reply's Data is a handful of command bytes a person wants to read,
            // and a hex string is exactly what jsonToCapnp accepts back.
            if (options.data_hex_limit != 0 && bytes.size() <= options.data_hex_limit)
            {
                return helpers::toHex(bytes);
            }

            // Otherwise the length rather than an invented encoding: the
            // payloads this hits by default are H.264 access units and PCM
            // audio, which nothing downstream would want inline anyway.
            json out = json::object();
            out["_data_bytes"] = bytes.size();
            if (options.data_hex_limit != 0)
            {
                out["hex_prefix"] = helpers::toHex(bytes.first(options.data_hex_limit));
            }
            return out;
        }
        case capnp::DynamicValue::LIST:
            return listToJson(value.as<capnp::DynamicList>(), options);
        case capnp::DynamicValue::ENUM:
        {
            auto enumerant = value.as<capnp::DynamicEnum>().getEnumerant();
            KJ_IF_MAYBE(e, enumerant)
            {
                return std::string(e->getProto().getName().cStr());
            }
            // An enumerant the schema does not know: report the raw value rather
            // than dropping it, since that is exactly the case worth seeing.
            return value.as<capnp::DynamicEnum>().getRaw();
        }
        case capnp::DynamicValue::STRUCT:
            return structToJson(value.as<capnp::DynamicStruct>(), options);
        case capnp::DynamicValue::CAPABILITY:
            return "<capability>";
        case capnp::DynamicValue::ANY_POINTER:
            return "<anyPointer>";
        case capnp::DynamicValue::UNKNOWN:
            return nullptr;
    }
    return nullptr;
}

std::string joinFieldNames(capnp::StructSchema::FieldList fields)
{
    std::string out;
    for (auto field : fields)
    {
        out += (out.empty() ? "" : ", ");
        out += field.getProto().getName().cStr();
    }
    return out;
}

// The inclusive range a capnp integer type can hold, as the widest types that
// can carry either end. Unsigned minimums are 0 and signed maximums fit int64.
struct IntegerRange
{
    bool is_signed;
    std::int64_t min;
    std::uint64_t max;
};

// nullopt for anything that is not an integer. Every enumerator is spelled out
// and there is no default, so a capnp release adding a type kind fails the
// build here (-Wswitch-enum) instead of silently reading as "not an integer".
std::optional<IntegerRange> integerRange(capnp::schema::Type::Which which)
{
    using W = capnp::schema::Type::Which;
    switch (which)
    {
        case W::INT8:   return IntegerRange{true, INT8_MIN, INT8_MAX};
        case W::INT16:  return IntegerRange{true, INT16_MIN, INT16_MAX};
        case W::INT32:  return IntegerRange{true, INT32_MIN, INT32_MAX};
        case W::INT64:  return IntegerRange{true, INT64_MIN, INT64_MAX};
        case W::UINT8:  return IntegerRange{false, 0, UINT8_MAX};
        case W::UINT16: return IntegerRange{false, 0, UINT16_MAX};
        case W::UINT32: return IntegerRange{false, 0, UINT32_MAX};
        case W::UINT64: return IntegerRange{false, 0, UINT64_MAX};

        case W::VOID:
        case W::BOOL:
        case W::FLOAT32:
        case W::FLOAT64:
        case W::TEXT:
        case W::DATA:
        case W::LIST:
        case W::ENUM:
        case W::STRUCT:
        case W::INTERFACE:
        case W::ANY_POINTER:
            return std::nullopt;
    }
    return std::nullopt;
}

void setValue(capnp::Type type, const json& value, const std::string& path,
              std::vector<std::string>& errors,
              const std::function<void(const capnp::DynamicValue::Reader&)>& set,
              const std::function<capnp::DynamicStruct::Builder()>& init_struct,
              const std::function<capnp::DynamicList::Builder(unsigned)>& init_list);

// One element of a list, of any element type. The recursion that makes
// List(List(UInt64)) and List(Struct) work the same way a field does.
void setListElement(capnp::DynamicList::Builder list, unsigned index, capnp::Type element_type,
                    const json& element, const std::string& path, std::vector<std::string>& errors)
{
    setValue(
        element_type, element, path, errors,
        [&](const capnp::DynamicValue::Reader& v) { list.set(index, v); },
        [&]() { return list[index].as<capnp::DynamicStruct>(); },
        [&](unsigned size) { return list.init(index, size).as<capnp::DynamicList>(); });
}

// Sets one value of type `type` from JSON, through whichever of the three
// callbacks that type needs. Shared by struct fields and list elements, so a
// list of enums is checked exactly the way an enum field is -- they used to be
// separate code, and the list half pushed every non-text, non-bool element
// through a double, which accepted a negative into a UInt8 list and rounded
// uint64 values above 2^53.
void setValue(capnp::Type type, const json& value, const std::string& path,
              std::vector<std::string>& errors,
              const std::function<void(const capnp::DynamicValue::Reader&)>& set,
              const std::function<capnp::DynamicStruct::Builder()>& init_struct,
              const std::function<capnp::DynamicList::Builder(unsigned)>& init_list)
{
    using W = capnp::schema::Type::Which;
    const W which = type.which();

    switch (which)
    {
        case W::VOID:
        {
            // Presence is the whole value -- a union arm chosen with no payload.
            // Setting it is what moves the discriminant.
            if (!value.is_null() && !(value.is_boolean() && value.get<bool>()) &&
                !(value.is_object() && value.empty()))
            {
                errors.push_back(path + ": a Void value takes null, true or {}.");
                return;
            }
            set(capnp::DynamicValue::Reader(capnp::VOID));
            return;
        }

        case W::BOOL:
        {
            if (!value.is_boolean())
            {
                errors.push_back(path + ": expected a boolean.");
                return;
            }
            set(value.get<bool>());
            return;
        }

        case W::INT8:
        case W::INT16:
        case W::INT32:
        case W::INT64:
        case W::UINT8:
        case W::UINT16:
        case W::UINT32:
        case W::UINT64:
        {
            const IntegerRange range = *integerRange(which);
            if (!value.is_number_integer())
            {
                errors.push_back(path + ": expected an integer.");
                return;
            }

            // Unsigned JSON values are read as unsigned and signed as signed, so
            // neither end of a 64-bit range goes through a conversion that loses
            // it. A negative number into an unsigned field is refused here: capnp
            // would otherwise throw, or, through a double, wrap it into something
            // plausible and wrong.
            if (value.is_number_unsigned())
            {
                const std::uint64_t v = value.get<std::uint64_t>();
                if (v > range.max)
                {
                    errors.push_back(path + ": " + std::to_string(v) + " is out of range (max " +
                                     std::to_string(range.max) + ").");
                    return;
                }
                if (range.is_signed)
                {
                    set(static_cast<std::int64_t>(v));
                }
                else
                {
                    set(v);
                }
                return;
            }

            const std::int64_t v = value.get<std::int64_t>();
            if (!range.is_signed && v < 0)
            {
                errors.push_back(path + ": expected a non-negative integer.");
                return;
            }
            if (v < range.min || (v >= 0 && static_cast<std::uint64_t>(v) > range.max))
            {
                errors.push_back(path + ": " + std::to_string(v) + " is out of range (" +
                                 std::to_string(range.min) + " to " + std::to_string(range.max) +
                                 ").");
                return;
            }
            if (range.is_signed)
            {
                set(v);
            }
            else
            {
                set(static_cast<std::uint64_t>(v));
            }
            return;
        }

        case W::FLOAT32:
        case W::FLOAT64:
        {
            if (!value.is_number())
            {
                errors.push_back(path + ": expected a number.");
                return;
            }
            const double v = value.get<double>();
            if (which == W::FLOAT32 && std::isfinite(v) &&
                std::fabs(v) > static_cast<double>(std::numeric_limits<float>::max()))
            {
                errors.push_back(path + ": " + value.dump() + " does not fit a Float32.");
                return;
            }
            set(v);
            return;
        }

        case W::TEXT:
        {
            if (!value.is_string())
            {
                errors.push_back(path + ": expected a string.");
                return;
            }
            const std::string text = value.get<std::string>();
            set(capnp::Text::Reader(text.c_str(), text.size()));
            return;
        }

        case W::DATA:
        {
            if (!value.is_string())
            {
                errors.push_back(path + ": expected a hex string, e.g. \"01 ff 7a\".");
                return;
            }
            std::string error;
            const auto bytes = helpers::fromHex(value.get<std::string>(), &error);
            if (!bytes)
            {
                errors.push_back(path + ": " + error);
                return;
            }
            set(capnp::Data::Reader(bytes->data(), bytes->size()));
            return;
        }

        case W::ENUM:
        {
            if (!value.is_string())
            {
                errors.push_back(path + ": expected an enumerant name.");
                return;
            }
            const auto schema = type.asEnum();
            const std::string wanted = value.get<std::string>();
            for (auto enumerant : schema.getEnumerants())
            {
                if (wanted == enumerant.getProto().getName().cStr())
                {
                    set(capnp::DynamicEnum(enumerant));
                    return;
                }
            }
            std::string valid;
            for (auto enumerant : schema.getEnumerants())
            {
                valid += (valid.empty() ? "" : ", ");
                valid += enumerant.getProto().getName().cStr();
            }
            errors.push_back(path + ": '" + wanted + "' is not valid. Expected one of: " + valid +
                             ".");
            return;
        }

        case W::STRUCT:
        {
            if (!value.is_object())
            {
                errors.push_back(path + ": expected an object.");
                return;
            }
            std::vector<std::string> nested;
            jsonToCapnp(value, init_struct(), nested);
            for (auto& error : nested)
            {
                errors.push_back(path + "." + error);
            }
            return;
        }

        case W::LIST:
        {
            if (!value.is_array())
            {
                errors.push_back(path + ": expected an array.");
                return;
            }
            auto list = init_list(static_cast<unsigned>(value.size()));
            const auto element_type = type.asList().getElementType();
            for (std::size_t i = 0; i < value.size(); ++i)
            {
                setListElement(list, static_cast<unsigned>(i), element_type, value[i],
                               path + "[" + std::to_string(i) + "]", errors);
            }
            return;
        }

        // No service in the tree takes either, and there is no JSON spelling
        // that would mean anything for them.
        case W::INTERFACE:
        case W::ANY_POINTER:
            errors.push_back(path + ": fields of this type cannot be set from JSON.");
            return;
    }

    // Only a value outside the enum reaches here -- a schema from a newer capnp
    // read by this one.
    errors.push_back(path + ": unhandled field type.");
}

}  // namespace

json capnpToJson(const std::vector<std::uint8_t>& bytes, capnp::Schema schema)
{
    return capnpToJson(bytes, schema, CapnpJsonOptions{});
}

json capnpToJson(const std::vector<std::uint8_t>& bytes, capnp::Schema schema,
                 const CapnpJsonOptions& options)
{
    // This used to copy into a word-aligned buffer unconditionally, which was
    // safe but paid for a heap allocation on every sample. WordAlignedPayload
    // only copies when the payload really is misaligned.
    const WordAlignedPayload aligned(bytes);

    // The old code divided the length down to whole words and decoded the
    // remainder, so a truncated payload came back as a JSON object full of
    // defaults -- indistinguishable from a healthy message reading zero. This is
    // the "throws kj::Exception on a malformed message" the header promises.
    KJ_REQUIRE(!aligned.empty(), "payload is not a whole number of capnp words",
               bytes.size(), sizeof(capnp::word));

    capnp::FlatArrayMessageReader reader(aligned.words());
    return structToJson(reader.getRoot<capnp::DynamicStruct>(schema.asStruct()), options);
}

json capnpToJson(capnp::DynamicStruct::Reader reader, const CapnpJsonOptions& options)
{
    return structToJson(reader, options);
}

bool jsonToCapnp(const json& value, capnp::DynamicStruct::Builder builder,
                 std::vector<std::string>& errors)
{
    if (!value.is_object())
    {
        errors.push_back("expected a JSON object.");
        return false;
    }

    const auto schema = builder.getSchema();
    const std::size_t before = errors.size();

    // A union is a group whose every field is an arm, and exactly one arm can be
    // set. Naming two would have the second silently replace the first, which is
    // the unknown-field problem again in a different shape.
    if (schema.getProto().getStruct().getDiscriminantCount() > 0 && value.size() != 1)
    {
        errors.push_back(std::string("a union takes exactly one of: ") +
                         joinFieldNames(schema.getFields()) + ".");
        return false;
    }

    for (const auto& [key, field_value] : value.items())
    {
        KJ_IF_MAYBE(field, schema.findFieldByName(key))
        {
            const capnp::StructSchema::Field f = *field;
            try
            {
                setValue(
                    f.getType(), field_value, key, errors,
                    [&](const capnp::DynamicValue::Reader& v) { builder.set(f, v); },
                    // init() on a group clears it and returns the builder over
                    // the parent's own storage, so plain groups and unions
                    // recurse exactly like a struct field does.
                    [&]() { return builder.init(f).as<capnp::DynamicStruct>(); },
                    [&](unsigned size) { return builder.init(f, size).as<capnp::DynamicList>(); });
            }
            catch (const kj::Exception& e)
            {
                // Anything capnp itself refuses that the checks above did not
                // anticipate. Reported, never thrown at a caller that asked for
                // a list of errors.
                errors.push_back(key + ": " + e.getDescription().cStr());
            }
        }
        else
        {
            errors.push_back(key + ": no such field. Known fields: " +
                             joinFieldNames(schema.getFields()) + ".");
        }
    }

    return errors.size() == before;
}

namespace
{

// The one place a capnp type becomes the category string every picker in the
// tree reasons about. Shared by a field and by a list's ELEMENTS, so the two can
// never disagree about what "float" means.
//
// A nested list reports "list" and stops there. Nothing in this tree publishes
// one, and an expression could not index it anyway -- exprtk vectors are flat.
std::string typeCategory(const capnp::Type& type)
{
    switch (type.which())
    {
        case capnp::schema::Type::BOOL:    return "bool";
        case capnp::schema::Type::INT8:
        case capnp::schema::Type::INT16:
        case capnp::schema::Type::INT32:
        case capnp::schema::Type::INT64:   return "int";
        case capnp::schema::Type::UINT8:
        case capnp::schema::Type::UINT16:
        case capnp::schema::Type::UINT32:
        case capnp::schema::Type::UINT64:  return "uint";
        case capnp::schema::Type::FLOAT32:
        case capnp::schema::Type::FLOAT64: return "float";
        case capnp::schema::Type::TEXT:    return "text";
        case capnp::schema::Type::DATA:    return "data";
        case capnp::schema::Type::LIST:    return "list";
        case capnp::schema::Type::STRUCT:  return "struct";
        case capnp::schema::Type::VOID:    return "void";
        case capnp::schema::Type::ENUM:    return "enum";
        case capnp::schema::Type::INTERFACE:
        case capnp::schema::Type::ANY_POINTER:
        default:                           return "other";
    }
}

// The enumerant names, in declaration order -- so index N is the name of the
// value an expression reading that field evaluates to. That correspondence is
// the whole contract: the evaluator hands out the ordinal, and this is what
// turns it back into something readable on an axis.
json enumerantNames(const capnp::EnumSchema& schema)
{
    json values = json::array();
    for (auto enumerant : schema.getEnumerants())
    {
        values.push_back(std::string(enumerant.getProto().getName().cStr()));
    }
    return values;
}

}  // namespace

std::optional<std::uint32_t> fixedListLength(const capnp::StructSchema::Field& field)
{
    for (auto annotation : field.getProto().getAnnotations())
    {
        if (annotation.getId() == kFixedLengthAnnotationId && annotation.getValue().isUint32())
        {
            return annotation.getValue().getUint32();
        }
    }
    return std::nullopt;
}

json describeSchema(capnp::Schema schema)
{
    json fields = json::object();
    for (auto field : schema.asStruct().getFields())
    {
        json entry = json::object();
        const auto type = field.getType();

        entry["type"] = typeCategory(type);

        if (type.isList())
        {
            // WHAT THE ELEMENTS ARE, because "list" on its own does not say
            // whether a consumer can do anything with it. A List(Float32) is 32
            // plottable channels; a List(Text) is not plottable at all, and a
            // picker that cannot tell them apart either offers both or neither.
            // `values[7]` is an ordinary expression, so this is the only thing
            // standing between the PDM's per-output data and a plot.
            const auto element = type.asList().getElementType();
            entry["element_type"] = typeCategory(element);

            // How many elements, when the schema says. A picker can then offer
            // each one as its own bindable channel instead of guessing a count
            // or peeking at live traffic -- see schemas/annotations.capnp.
            if (const auto length = fixedListLength(field))
            {
                entry["fixed_length"] = *length;
            }

            if (element.isEnum())
            {
                entry["values"] = enumerantNames(element.asEnum());
            }
        }
        else if (type.isEnum())
        {
            entry["values"] = enumerantNames(type.asEnum());
        }

        fields[field.getProto().getName().cStr()] = std::move(entry);
    }

    json out = json::object();
    out["fields"] = std::move(fields);
    return out;
}

}  // namespace pub_sub
