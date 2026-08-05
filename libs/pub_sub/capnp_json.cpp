#include "pub_sub/capnp_json.h"

#include "pub_sub/capnp_payload.h"

#include <capnp/serialize.h>

#include <limits>

namespace pub_sub
{

namespace
{

json dynamicToJson(capnp::DynamicValue::Reader value);

json listToJson(capnp::DynamicList::Reader list)
{
    json out = json::array();
    for (auto element : list)
    {
        out.push_back(dynamicToJson(element));
    }
    return out;
}

json structToJson(capnp::DynamicStruct::Reader reader)
{
    json out = json::object();

    // Non-union fields plus whichever union arm is active. Asking for an
    // inactive union field throws, so this is the only safe traversal.
    for (auto field : reader.getSchema().getNonUnionFields())
    {
        out[field.getProto().getName().cStr()] = dynamicToJson(reader.get(field));
    }

    // KJ_IF_MAYBE binds a pointer, not a reference, in this capnp version.
    KJ_IF_MAYBE(active, reader.which())
    {
        out[active->getProto().getName().cStr()] = dynamicToJson(reader.get(*active));
    }

    return out;
}

json dynamicToJson(capnp::DynamicValue::Reader value)
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
            // Bytes are not JSON. Report the length rather than inventing an
            // encoding: the payloads this hits are H.264 access units and PCM
            // audio, which nothing downstream would want inline anyway.
            const auto data = value.as<capnp::Data>();
            json out = json::object();
            out["_data_bytes"] = data.size();
            return out;
        }
        case capnp::DynamicValue::LIST:
            return listToJson(value.as<capnp::DynamicList>());
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
            return structToJson(value.as<capnp::DynamicStruct>());
        case capnp::DynamicValue::CAPABILITY:
            return "<capability>";
        case capnp::DynamicValue::ANY_POINTER:
            return "<anyPointer>";
        case capnp::DynamicValue::UNKNOWN:
            return nullptr;
    }
    return nullptr;
}

std::string joinFieldNames(capnp::StructSchema schema)
{
    std::string out;
    for (auto field : schema.getFields())
    {
        out += (out.empty() ? "" : ", ");
        out += field.getProto().getName().cStr();
    }
    return out;
}

void setField(capnp::DynamicStruct::Builder builder, capnp::StructSchema::Field field,
              const json& value, const std::string& path, std::vector<std::string>& errors)
{
    const auto type = field.getType();

    switch (type.which())
    {
        case capnp::schema::Type::BOOL:
            if (!value.is_boolean())
            {
                errors.push_back(path + ": expected a boolean.");
                return;
            }
            builder.set(field, value.get<bool>());
            return;

        case capnp::schema::Type::INT8:
        case capnp::schema::Type::INT16:
        case capnp::schema::Type::INT32:
        case capnp::schema::Type::INT64:
            if (!value.is_number_integer())
            {
                errors.push_back(path + ": expected an integer.");
                return;
            }
            builder.set(field, value.get<std::int64_t>());
            return;

        case capnp::schema::Type::UINT8:
        case capnp::schema::Type::UINT16:
        case capnp::schema::Type::UINT32:
        case capnp::schema::Type::UINT64:
            if (!value.is_number_unsigned())
            {
                // Rejecting a negative number here matters: capnp would wrap it
                // into a huge positive value, and a gauge would then display
                // something plausible and wrong.
                errors.push_back(path + ": expected a non-negative integer.");
                return;
            }
            builder.set(field, value.get<std::uint64_t>());
            return;

        case capnp::schema::Type::FLOAT32:
        case capnp::schema::Type::FLOAT64:
            if (!value.is_number())
            {
                errors.push_back(path + ": expected a number.");
                return;
            }
            builder.set(field, value.get<double>());
            return;

        case capnp::schema::Type::TEXT:
            if (!value.is_string())
            {
                errors.push_back(path + ": expected a string.");
                return;
            }
            builder.set(field, capnp::Text::Reader(value.get<std::string>().c_str()));
            return;

        case capnp::schema::Type::ENUM:
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
                    builder.set(field, capnp::DynamicEnum(enumerant));
                    return;
                }
            }
            std::string valid;
            for (auto enumerant : schema.getEnumerants())
            {
                valid += (valid.empty() ? "" : ", ");
                valid += enumerant.getProto().getName().cStr();
            }
            errors.push_back(path + ": '" + wanted + "' is not valid. Expected one of: " +
                             valid + ".");
            return;
        }

        case capnp::schema::Type::STRUCT:
        {
            if (!value.is_object())
            {
                errors.push_back(path + ": expected an object.");
                return;
            }
            std::vector<std::string> nested;
            jsonToCapnp(value, builder.init(field).as<capnp::DynamicStruct>(), nested);
            for (auto& error : nested)
            {
                errors.push_back(path + "." + error);
            }
            return;
        }

        case capnp::schema::Type::LIST:
        {
            if (!value.is_array())
            {
                errors.push_back(path + ": expected an array.");
                return;
            }
            auto list = builder.init(field, static_cast<unsigned int>(value.size()))
                            .as<capnp::DynamicList>();
            const auto element_type = type.asList().getElementType();
            for (std::size_t i = 0; i < value.size(); ++i)
            {
                const std::string element_path = path + "[" + std::to_string(i) + "]";
                const json& element = value[i];
                const auto index = static_cast<unsigned int>(i);

                // An if-chain, not a switch: capnp::schema::Type::Which has 18
                // enumerators and the build runs with -Wswitch-enum, so a switch
                // would mean listing all of them to handle four.
                if (element_type.which() == capnp::schema::Type::STRUCT)
                {
                    if (!element.is_object())
                    {
                        errors.push_back(element_path + ": expected an object.");
                        continue;
                    }
                    std::vector<std::string> nested;
                    jsonToCapnp(element, list[index].as<capnp::DynamicStruct>(), nested);
                    for (auto& error : nested)
                    {
                        errors.push_back(element_path + "." + error);
                    }
                }
                else if (element_type.which() == capnp::schema::Type::TEXT)
                {
                    if (!element.is_string())
                    {
                        errors.push_back(element_path + ": expected a string.");
                        continue;
                    }
                    list.set(index, capnp::Text::Reader(element.get<std::string>().c_str()));
                }
                else if (element_type.which() == capnp::schema::Type::BOOL)
                {
                    if (!element.is_boolean())
                    {
                        errors.push_back(element_path + ": expected a boolean.");
                        continue;
                    }
                    list.set(index, element.get<bool>());
                }
                else
                {
                    if (!element.is_number())
                    {
                        errors.push_back(element_path + ": expected a number.");
                        continue;
                    }
                    list.set(index, element.get<double>());
                }
            }
            return;
        }

        case capnp::schema::Type::VOID:
            // Nothing to set; presence is the whole value.
            return;

        case capnp::schema::Type::DATA:
        case capnp::schema::Type::INTERFACE:
        case capnp::schema::Type::ANY_POINTER:
            errors.push_back(path + ": fields of this type cannot be set from JSON.");
            return;
    }

    errors.push_back(path + ": unhandled field type.");
}

}  // namespace

json capnpToJson(const std::vector<std::uint8_t>& bytes, capnp::Schema schema)
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
    return structToJson(reader.getRoot<capnp::DynamicStruct>(schema.asStruct()));
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

    for (const auto& [key, field_value] : value.items())
    {
        KJ_IF_MAYBE(field, schema.findFieldByName(key))
        {
            setField(builder, *field, field_value, key, errors);
        }
        else
        {
            errors.push_back(key + ": no such field. Known fields: " +
                             joinFieldNames(schema) + ".");
        }
    }

    return errors.size() == before;
}

json describeSchema(capnp::Schema schema)
{
    json fields = json::object();
    for (auto field : schema.asStruct().getFields())
    {
        json entry = json::object();
        const auto type = field.getType();

        switch (type.which())
        {
            case capnp::schema::Type::BOOL:    entry["type"] = "bool";   break;
            case capnp::schema::Type::INT8:
            case capnp::schema::Type::INT16:
            case capnp::schema::Type::INT32:
            case capnp::schema::Type::INT64:   entry["type"] = "int";    break;
            case capnp::schema::Type::UINT8:
            case capnp::schema::Type::UINT16:
            case capnp::schema::Type::UINT32:
            case capnp::schema::Type::UINT64:  entry["type"] = "uint";   break;
            case capnp::schema::Type::FLOAT32:
            case capnp::schema::Type::FLOAT64: entry["type"] = "float";  break;
            case capnp::schema::Type::TEXT:    entry["type"] = "text";   break;
            case capnp::schema::Type::DATA:    entry["type"] = "data";   break;
            case capnp::schema::Type::LIST:    entry["type"] = "list";   break;
            case capnp::schema::Type::STRUCT:  entry["type"] = "struct"; break;
            case capnp::schema::Type::VOID:    entry["type"] = "void";   break;
            case capnp::schema::Type::ENUM:
            {
                entry["type"] = "enum";
                json values = json::array();
                for (auto enumerant : type.asEnum().getEnumerants())
                {
                    values.push_back(std::string(enumerant.getProto().getName().cStr()));
                }
                entry["values"] = std::move(values);
                break;
            }
            case capnp::schema::Type::INTERFACE:
            case capnp::schema::Type::ANY_POINTER:
            default:
                entry["type"] = "other";
                break;
        }

        fields[field.getProto().getName().cStr()] = std::move(entry);
    }

    json out = json::object();
    out["fields"] = std::move(fields);
    return out;
}

}  // namespace pub_sub
