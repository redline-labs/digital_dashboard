#include "pub_sub/zenoh_subscriber.h"

#include "spdlog/spdlog.h"
#include "helpers/unit_conversion.h"
#include "pub_sub/session_manager.h"
#include "pub_sub/schema_registry.h"
#include "reflection/reflection.h"

#include <stdexcept>
#include <algorithm>
#include <sstream>
#include <capnp/serialize.h>

namespace pub_sub
{

ZenohExpressionSubscriber::ZenohExpressionSubscriber(schema_type_t schema_type, const std::string& expression, const std::string& zenoh_key) :
    schema_type_{schema_type},
    expression_{expression},
    variables_{},
    is_valid_{false},
    symbol_table_{},
    compiled_expression_{},
    parser_{},
    field_cache_{},
    zenoh_key_{zenoh_key},
    zenoh_session_{},
    zenoh_subscriber_{},
    evaluation_handler_{}
{
    
    // Assume valid until proven otherwise
    is_valid_ = true;

    if (zenoh_key_.empty() || expression_.empty())
    {
        SPDLOG_ERROR("Zenoh key ({}) or expression ({}) is empty", zenoh_key_, expression_);
        is_valid_ = false;
        return;
    }

    // Get the schema from the registry
    schema_ = get_schema(schema_type_);
    
    // Check if schema was found
    if (schema_.getProto().getId() == 0)
    {
        SPDLOG_ERROR("Schema '{}' not found in registry", reflection::enum_traits<pub_sub::schema_type_t>::to_string(schema_type_));
        is_valid_ = false;
        return;
    }

    // Add the extend_functions to the symbol table.
    symbol_table_.add_function("mph_to_mps", &mph_to_mps<double>);
    symbol_table_.add_function("mps_to_mph", &mps_to_mph<double>);
    symbol_table_.add_function("psi_to_bar", &psi_to_bar<double>);
    symbol_table_.add_function("bar_to_psi", &bar_to_psi<double>);
    symbol_table_.add_function("celsius_to_fahrenheit", &celsius_to_fahrenheit<double>);
    symbol_table_.add_function("fahrenheit_to_celsius", &fahrenheit_to_celsius<double>);

    // Extract variables from the expression
    extractVariables();
    
    // Validate variables against schema
    validateVariablesAgainstSchema();

    if (is_valid_ == true)
    {
        // Build the field cache for fast extraction
        buildFieldCache();
        
        // Add the variables to the symbol table.
        for (auto& [var, value] : variables_)
        {
            symbol_table_.add_variable(var, value);
        }

        symbol_table_.add_constants();

        compiled_expression_.register_symbol_table(symbol_table_);
        is_valid_ = parser_.compile(expression_, compiled_expression_) == true;
    }


    try
    {
        zenoh_session_ = SessionManager::getOrCreate();
        if (!zenoh_session_)
        {
            SPDLOG_ERROR("No zenoh session available to subscribe to '{}'", zenoh_key_);
            return;
        }

        auto key_expr = zenoh::KeyExpr(zenoh_key_);
        zenoh_subscriber_ = std::make_unique<zenoh::Subscriber<void>>(
            zenoh_session_->declare_subscriber(
                key_expr,
                [this](const zenoh::Sample& sample)
                {
                    if (evaluation_handler_ == nullptr)
                    {
                        SPDLOG_ERROR("No evaluation handler set for key '{}'", zenoh_key_);
                        return;
                    }

                    try
                    {
                        std::vector<uint8_t> bytes = sample.get_payload().as_vector();
                        evaluation_handler_(bytes);
                    }
                    catch (const std::exception& e)
                    {
                        SPDLOG_ERROR("Error handling zenoh sample: {}", e.what());
                    }
                },
                zenoh::closures::none
            )
        );
        SPDLOG_DEBUG("Subscribed to zenoh key '{}'", zenoh_key_);
    }
    catch (const std::exception& e)
    {
        SPDLOG_ERROR("Failed to subscribe to key '{}': {}", zenoh_key_, e.what());
    }
}

void ZenohExpressionSubscriber::extractVariables()
{
    // Use ExprTk's collect_variables utility function
    std::vector<std::string> variable_list;
    
    if (exprtk::collect_variables(expression_, symbol_table_, variable_list))
    {
        // Store the variable list and sort it
        for (const auto& var : variable_list)
        {
            variables_[var] = 0.0;
        }
    }
    else
    {
        SPDLOG_ERROR("Failed to extract variables from expression '{}'", expression_);
        is_valid_ = false;
        variables_.clear();
    }
}

void ZenohExpressionSubscriber::validateVariablesAgainstSchema()
{
    // Get all field names from the schema
    auto schema_fields = getSchemaFieldNames(schema_);
    
    // Check each variable against the schema fields
    for (const auto& [var, _] : variables_)
    {
        if (schema_fields.find(var) == schema_fields.end())
        {
            SPDLOG_ERROR("Variable '{}' not found in schema '{}'", var, reflection::enum_traits<pub_sub::schema_type_t>::to_string(schema_type_));
            is_valid_ = false;
        }
    }
}

std::unordered_set<std::string> ZenohExpressionSubscriber::getSchemaFieldNames(const capnp::Schema& schema)
{
    std::unordered_set<std::string> field_names;
    
    if (schema.getProto().isStruct())
    {
        auto struct_schema = schema.asStruct();
        auto fields = struct_schema.getFields();
        
        for (auto field : fields)
        {
            field_names.insert(field.getProto().getName().cStr());
        }
    }
    
    return field_names;
}

const schema_type_t& ZenohExpressionSubscriber::getSchemaType() const
{
    return schema_type_;
}

const std::string& ZenohExpressionSubscriber::getExpression() const
{
    return expression_;
}

const capnp::Schema& ZenohExpressionSubscriber::getSchema() const
{
    return schema_;
}

const std::map<std::string, double>& ZenohExpressionSubscriber::getVariables() const
{
    return variables_;
}

bool ZenohExpressionSubscriber::isValid() const
{
    return is_valid_;
}

void ZenohExpressionSubscriber::buildFieldCache()
{
    field_cache_.clear();
    
    if (!schema_.getProto().isStruct())
    {
        SPDLOG_ERROR("Schema is not a struct, cannot build field cache");
        return;
    }
    
    auto struct_schema = schema_.asStruct();
    
    // Pre-compute field access information for all required variables
    for (const auto& [var_name, _] : variables_)
    {
        // Find the field in the schema
        auto field = struct_schema.getFieldByName(var_name.c_str());
        
        // Determine the expected type for optimized extraction
        capnp::DynamicValue::Type expected_type = capnp::DynamicValue::UNKNOWN;
        auto field_type = field.getType();
        
        if (field_type.isBool())
        {
            expected_type = capnp::DynamicValue::BOOL;
        }
        else if (field_type.isInt8() || field_type.isInt16() || field_type.isInt32() || field_type.isInt64())
        {
            expected_type = capnp::DynamicValue::INT;
        }
        else if (field_type.isUInt8() || field_type.isUInt16() || field_type.isUInt32() || field_type.isUInt64())
        {
            expected_type = capnp::DynamicValue::UINT;
        }
        else if (field_type.isFloat32() || field_type.isFloat64())
        {
            expected_type = capnp::DynamicValue::FLOAT;
        }
        else if (field_type.isText())
        {
            expected_type = capnp::DynamicValue::TEXT;
        }
        
        // Cache the field information
        field_cache_.push_back({field, var_name, expected_type});
    }
}

void ZenohExpressionSubscriber::extractFieldValues(capnp::DynamicStruct::Reader reader)
{
    // Use cached field information for fast extraction
    for (const auto& cached_field : field_cache_)
    {
        auto value = reader.get(cached_field.field);
        double numeric_value = 0.0;
        
        // Fast type conversion using cached type information
        switch (cached_field.expected_type)
        {
            case capnp::DynamicValue::BOOL:
                numeric_value = value.as<bool>() ? 1.0 : 0.0;
                break;

            case capnp::DynamicValue::INT:
                numeric_value = static_cast<double>(value.as<int64_t>());
                break;

            case capnp::DynamicValue::UINT:
                numeric_value = static_cast<double>(value.as<uint64_t>());
                break;

            case capnp::DynamicValue::FLOAT:
                numeric_value = static_cast<double>(value.as<double>());
                break;

            case capnp::DynamicValue::TEXT:
            case capnp::DynamicValue::UNKNOWN:
            case capnp::DynamicValue::VOID:
            case capnp::DynamicValue::DATA:
            case capnp::DynamicValue::LIST:
            case capnp::DynamicValue::ENUM:
            case capnp::DynamicValue::STRUCT:
            case capnp::DynamicValue::CAPABILITY:
            case capnp::DynamicValue::ANY_POINTER:
            default:
                SPDLOG_WARN("Skipping field '{}' in expression evaluation", cached_field.name);
                numeric_value = 0.0;
        }
        
        variables_[cached_field.name] = numeric_value;
    }
}

} // namespace pub_sub