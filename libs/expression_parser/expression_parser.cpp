#include "expression_parser/expression_parser.h"

#include "spdlog/spdlog.h"

#include <stdexcept>
#include <algorithm>
#include <sstream>

namespace expression_parser {

ExpressionParser::ExpressionParser(const std::string& schema_name, const std::string& expression):
    schema_name_{schema_name},
    expression_{expression},
    is_valid_{false},
    symbol_table_{},
    compiled_expression_{},
    parser_{}
{
    
    // Assume valid until proven otherwise
    is_valid_ = true;

    // Get the schema from the registry
    schema_ = get_schema_by_name(schema_name);
    
    // Check if schema was found
    if (schema_.getProto().getId() == 0)
    {
        SPDLOG_ERROR("Schema '{}' not found in registry", schema_name);
        is_valid_ = false;
        return;
    }
    
    // Extract variables from the expression
    extractVariables();
    
    // Validate variables against schema
    validateVariablesAgainstSchema();

    if (is_valid_ == true)
    {
        // Add the variables to the symbol table.
        for (auto& [var, value] : variables_)
        {
            symbol_table_.add_variable(var, value);
        }

        symbol_table_.add_constants();

        compiled_expression_.register_symbol_table(symbol_table_);
        is_valid_ = parser_.compile(expression_, compiled_expression_) == true;
    }

    if (is_valid_ == true)
    {
        SPDLOG_INFO("Expression '{}' compiled successfully", expression_);
    }
}

void ExpressionParser::extractVariables() {
    // Use ExprTk's collect_variables utility function
    std::vector<std::string> variable_list;
    
    if (exprtk::collect_variables(expression_, variable_list))
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

void ExpressionParser::validateVariablesAgainstSchema()
{
    // Get all field names from the schema
    auto schema_fields = getSchemaFieldNames(schema_);
    
    // Check each variable against the schema fields
    for (const auto& [var, _] : variables_)
    {
        if (schema_fields.find(var) == schema_fields.end())
        {
            SPDLOG_ERROR("Variable '{}' not found in schema '{}'", var, schema_name_);
            is_valid_ = false;
        }
    }
}

std::unordered_set<std::string> ExpressionParser::getSchemaFieldNames(const capnp::Schema& schema)
{
    std::unordered_set<std::string> field_names;
    
    if (schema.getProto().isStruct())
    {
        auto struct_schema = schema.asStruct();
        auto fields = struct_schema.getFields();
        
        for (auto field : fields)
        {
            field_names.insert(field.getProto().getName());
        }
    }
    
    return field_names;
}

const std::string& ExpressionParser::getSchemaName() const
{
    return schema_name_;
}

const std::string& ExpressionParser::getExpression() const
{
    return expression_;
}

const capnp::Schema& ExpressionParser::getSchema() const
{
    return schema_;
}

const std::map<std::string, double>& ExpressionParser::getVariables() const
{
    return variables_;
}

bool ExpressionParser::isValid() const
{
    return is_valid_;
}

} // namespace expression_parser