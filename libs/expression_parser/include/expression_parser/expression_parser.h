#ifndef EXPRESSION_PARSER_H
#define EXPRESSION_PARSER_H

#include <string>
#include <map>
#include <unordered_set>
#include <memory>

#include <capnp/schema.h>
#include <capnp/dynamic.h>
#include <capnp/message.h>
#include <capnp/serialize.h>
#include <exprtk.hpp>

#include "expression_parser/schema_registry.h"

namespace expression_parser
{

/**
 * ExpressionParser class responsible for parsing and evaluating expressions
 * against Cap'n Proto schema fields.
 * 
 * This class:
 * - Accepts a schema name and expression during construction
 * - Validates that all variables in the expression exist in the schema
 * - Provides functionality to evaluate expressions against message data
 */
class ExpressionParser
{
  public:
    /**
     * Constructor that initializes the parser with a schema and expression
     * 
     * @param schema_name Name of the schema to use for validation
     * @param expression Mathematical expression string to be evaluated
     */
    ExpressionParser(const std::string& schema_name, const std::string& expression);

    /**
     * Get the schema name being used
     * @return The schema name
     */
    const std::string& getSchemaName() const;

    /**
     * Get the expression string
     * @return The expression string
     */
    const std::string& getExpression() const;

    /**
     * Get the list of variables extracted from the expression
     * @return Map of variable names and their values
     */
    const std::map<std::string, double>& getVariables() const;

    /**
     * Get the Cap'n Proto schema object
     * @return The schema object
     */
    const capnp::Schema& getSchema() const;

    /**
     * Check if the expression is valid and all variables exist in the schema
     * @return true if valid, false otherwise
     */
    bool isValid() const;

    /**
     * Evaluate the expression against a Cap'n Proto message payload
     * @param payload Raw Cap'n Proto message bytes
     * @param size Size of the payload in bytes
     * @return The evaluated result as a double
     * @throws std::runtime_error if evaluation fails
     */
    double evaluate(const void* payload, size_t size);

    /**
     * Evaluate the expression against a Cap'n Proto message payload
     * @param payload Vector containing Cap'n Proto message bytes
     * @return The evaluated result as a double
     * @throws std::runtime_error if evaluation fails
     */
    double evaluate(const std::vector<uint8_t>& payload);

private:
    // Cached field information for fast extraction
    struct FieldCache {
        capnp::StructSchema::Field field;
        std::string name;
        capnp::DynamicValue::Type expected_type;
    };

    std::string schema_name_;
    std::string expression_;
    capnp::Schema schema_;
    std::map<std::string, double> variables_;
    bool is_valid_;

    // exprtk components
    exprtk::symbol_table<double> symbol_table_;
    exprtk::expression<double> compiled_expression_;
    exprtk::parser<double> parser_;

    // Field extraction cache - pre-computed field access information
    std::vector<FieldCache> field_cache_;

    /**
     * Extract variables from the expression using exprtk
     */
    void extractVariables();

    /**
     * Validate that all extracted variables exist in the schema
     */
    void validateVariablesAgainstSchema();

    /**
     * Get all field names from a Cap'n Proto schema
     * @param schema The schema to examine
     * @return Set of field names
     */
    std::unordered_set<std::string> getSchemaFieldNames(const capnp::Schema& schema);

    /**
     * Build the field cache for fast extraction during evaluation
     * This pre-computes field access information for all required variables
     */
    void buildFieldCache();

    /**
     * Extract field values from a Cap'n Proto message reader using cached field info
     * @param reader The message reader
     * @return Map of field names to their numeric values
     */
    void extractFieldValues(capnp::DynamicStruct::Reader reader);
};

} // namespace expression_parser

#endif // EXPRESSION_PARSER_H