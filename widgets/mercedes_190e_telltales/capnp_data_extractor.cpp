#include "mercedes_190e_telltales/capnp_data_extractor.h"

#include <spdlog/spdlog.h>
#include <algorithm>
#include <cctype>

// Include all known schemas
#include "vehicle_warnings.capnp.h"
#include "engine_rpm.capnp.h"
#include "engine_temperature.capnp.h"
#include "vehicle_speed.capnp.h"

void CapnpDataExtractor::setSchemaType(const std::string& schema_type)
{
    _schema_type = schema_type;
    initializeSchema();
}

void CapnpDataExtractor::initializeSchema()
{
    try {
        // Map schema type strings to actual schemas
        if (_schema_type == "BatteryWarning") {
            _schema = capnp::Schema::from<BatteryWarning>();
        }
        else if (_schema_type == "EngineRpm") {
            _schema = capnp::Schema::from<EngineRpm>();
        }
        else if (_schema_type == "EngineTemperature") {
            _schema = capnp::Schema::from<EngineTemperature>();
        }
        else if (_schema_type == "VehicleSpeed") {
            _schema = capnp::Schema::from<VehicleSpeed>();
        }
        else {
            SPDLOG_ERROR("CapnpDataExtractor: Unknown schema type '{}'", _schema_type);
            return;
        }
        
        SPDLOG_INFO("CapnpDataExtractor: Initialized schema for type '{}'", _schema_type);
        
    } catch (const std::exception& e) {
        SPDLOG_ERROR("CapnpDataExtractor: Failed to initialize schema for '{}': {}", 
                     _schema_type, e.what());
    }
}

CapnpDataExtractor::VariableMap CapnpDataExtractor::extractVariables(const std::string& payload) const
{
    VariableMap variables;
    
    if (!_schema.getProto().isStruct()) {
        SPDLOG_ERROR("CapnpDataExtractor: Schema is not a struct type");
        return variables;
    }
    
    try {
        // Deserialize the message
        ::capnp::FlatArrayMessageReader message(
            kj::arrayPtr(reinterpret_cast<const capnp::word*>(payload.data()),
                       payload.size() / sizeof(capnp::word))
        );
        
        // Get the root as AnyPointer and then convert to DynamicStruct
        auto root = message.getRoot<capnp::AnyPointer>();
        capnp::DynamicStruct::Reader reader = root.getAs<capnp::DynamicStruct>(_schema.asStruct());
        
        // Extract variables from the struct
        extractFromStruct(reader, variables);
        
    } catch (const std::exception& e) {
        SPDLOG_ERROR("CapnpDataExtractor: Failed to extract variables: {}", e.what());
    }
    
    return variables;
}

std::vector<std::string> CapnpDataExtractor::getAvailableVariables() const
{
    std::vector<std::string> variable_names;
    
    if (!_schema.getProto().isStruct()) {
        return variable_names;
    }
    
    try {
        auto struct_schema = _schema.asStruct();
        auto fields = struct_schema.getFields();
        
        for (auto field : fields) {
            std::string field_name = field.getProto().getName();
            std::string var_name = fieldToVariableName(field_name);
            
            auto field_type = field.getType();
            
            // Only include numeric types
            if (field_type.isUInt8() || field_type.isUInt16() || field_type.isUInt32() || field_type.isUInt64() ||
                field_type.isInt8() || field_type.isInt16() || field_type.isInt32() || field_type.isInt64() ||
                field_type.isFloat32() || field_type.isFloat64() || field_type.isBool()) {
                variable_names.push_back(var_name);
            }
        }
        
    } catch (const std::exception& e) {
        SPDLOG_ERROR("CapnpDataExtractor: Failed to get available variables: {}", e.what());
    }
    
    return variable_names;
}

void CapnpDataExtractor::extractFromStruct(const capnp::DynamicStruct::Reader& reader, 
                                          VariableMap& variables, 
                                          const std::string& prefix) const
{
    auto schema = reader.getSchema();
    auto fields = schema.getFields();
    
    for (auto field : fields) {
        try {
            std::string field_name = field.getProto().getName();
            std::string var_name = prefix.empty() ? 
                fieldToVariableName(field_name) : 
                prefix + "_" + fieldToVariableName(field_name);
            
            auto field_type = field.getType();
            
            if (!reader.has(field)) {
                continue; // Skip unset fields
            }
            
            // Extract numeric values
            if (field_type.isBool()) {
                variables[var_name] = reader.get(field).as<bool>() ? 1.0 : 0.0;
            }
            else if (field_type.isUInt8()) {
                variables[var_name] = static_cast<double>(reader.get(field).as<uint8_t>());
            }
            else if (field_type.isUInt16()) {
                variables[var_name] = static_cast<double>(reader.get(field).as<uint16_t>());
            }
            else if (field_type.isUInt32()) {
                variables[var_name] = static_cast<double>(reader.get(field).as<uint32_t>());
            }
            else if (field_type.isUInt64()) {
                variables[var_name] = static_cast<double>(reader.get(field).as<uint64_t>());
            }
            else if (field_type.isInt8()) {
                variables[var_name] = static_cast<double>(reader.get(field).as<int8_t>());
            }
            else if (field_type.isInt16()) {
                variables[var_name] = static_cast<double>(reader.get(field).as<int16_t>());
            }
            else if (field_type.isInt32()) {
                variables[var_name] = static_cast<double>(reader.get(field).as<int32_t>());
            }
            else if (field_type.isInt64()) {
                variables[var_name] = static_cast<double>(reader.get(field).as<int64_t>());
            }
            else if (field_type.isFloat32()) {
                variables[var_name] = static_cast<double>(reader.get(field).as<float>());
            }
            else if (field_type.isFloat64()) {
                variables[var_name] = reader.get(field).as<double>();
            }
            else if (field_type.isStruct()) {
                // Recursively extract from nested structs
                auto nested_reader = reader.get(field).as<capnp::DynamicStruct>();
                extractFromStruct(nested_reader, variables, var_name);
            }
            // Skip other types (text, data, lists, etc.)
            
        } catch (const std::exception& e) {
            SPDLOG_WARN("CapnpDataExtractor: Failed to extract field '{}': {}", 
                       field.getProto().getName().cStr(), e.what());
        }
    }
}

std::string CapnpDataExtractor::fieldToVariableName(const std::string& field_name) const
{
    // Convert camelCase or snake_case to a clean variable name
    std::string result = field_name;
    
    // Convert to camelCase if it's snake_case
    bool next_upper = false;
    for (size_t i = 0; i < result.length(); ++i) {
        if (result[i] == '_') {
            next_upper = true;
            result.erase(i, 1);
            --i;
        } else if (next_upper) {
            result[i] = std::toupper(result[i]);
            next_upper = false;
        }
    }
    
    return result;
} 