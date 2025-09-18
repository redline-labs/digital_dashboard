#ifndef ZENOH_SUBSCRIBER_H_
#define ZENOH_SUBSCRIBER_H_

#include <string>
#include <map>
#include <unordered_set>
#include <memory>
#include <type_traits>
#include <cmath>
#include <functional>
#include <vector>

#include <capnp/schema.h>
#include <capnp/dynamic.h>
#include <capnp/message.h>
#include <capnp/serialize.h>
#include "zenoh.hxx"
#define exprtk_disable_caseinsensitivity
#include <exprtk.hpp>

#include "pub_sub/schema_registry.h"
#include "spdlog/spdlog.h"

namespace pub_sub
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
class ZenohExpressionSubscriber
{
  public:
    /**
     * Constructor that additionally stores a Zenoh key for automatic subscription
     * using the process-wide SessionManager.
     *
     * @param schema_name Name of the schema to use for validation
     * @param expression Mathematical expression string to be evaluated
     * @param zenoh_key Key expression to subscribe to via Zenoh
     */
    ZenohExpressionSubscriber(schema_type_t schema_type, const std::string& expression, const std::string& zenoh_key);

    /**
     * Get the schema type being used
     * @return The schema type
     */
    const schema_type_t& getSchemaType() const;

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
     * Configure the result callback that will be invoked when subscribed data
     * is received and evaluated. When set, the parser will subscribe using
     * the process-wide SessionManager with the provided zenoh_key.
     */
    template<typename T>
    void setResultCallback(std::function<void(T)> callback)
    {
        evaluation_handler_ = [this, cb = std::move(callback)](const std::vector<uint8_t>& payload) mutable
        {
            try
            {
                T value = this->evaluate<T>(payload);
                cb(value);
            }
            catch (const std::exception& e)
            {
                SPDLOG_ERROR("Evaluation callback error: {}", e.what());
            }
        };
    }

  private:
    // Cached field information for fast extraction
    struct FieldCache
    {
        capnp::StructSchema::Field field;
        std::string name;
        capnp::DynamicValue::Type expected_type;
    };

    schema_type_t schema_type_;
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

    // Zenoh related configuration/handles
    std::string zenoh_key_;
    std::shared_ptr<zenoh::Session> zenoh_session_;
    std::unique_ptr<zenoh::Subscriber<void>> zenoh_subscriber_;
    std::function<void(const std::vector<uint8_t>&)> evaluation_handler_;

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

    /**
     * Evaluate the expression against a Cap'n Proto message payload with templated return type
     * @tparam T The desired return type (e.g., float, bool, int)
     * @param payload Raw Cap'n Proto message bytes
     * @return The evaluated result cast to type T
     */
    template<typename T>
    T evaluate(const std::vector<uint8_t>& payload)
    {
        if (!is_valid_)
        {
            SPDLOG_ERROR("Expression is not valid, cannot evaluate.");
            return 0.0;
        }
    
        try
        {
            // Create a Cap'n Proto message reader from the raw payload
            capnp::FlatArrayMessageReader message_reader(
                kj::arrayPtr(reinterpret_cast<const capnp::word*>(payload.data()), payload.size() / sizeof(capnp::word)));
            
            // Get the root as a dynamic struct using our schema
            auto root = message_reader.getRoot<capnp::DynamicStruct>(schema_.asStruct());
            
            // Extract field values from the message
            extractFieldValues(root);
            
            // Evaluate the compiled expression and cast to desired type
            double result = compiled_expression_.value();
            
            // Handle different return types with appropriate conversions
            if constexpr (std::is_same_v<T, bool>)
            {
                // For boolean, consider anything non-zero as true
                return static_cast<T>(result != 0.0);
            }
            else if constexpr (std::is_integral_v<T>)
            {
                // For integer types, round to nearest integer
                return static_cast<T>(std::round(result));
            }
            else
            {
                // For floating point types, direct cast
                return static_cast<T>(result);
            }
        }
        catch (const std::exception& e)
        {
            SPDLOG_ERROR("Expression evaluation failed: {}", e.what());
            return 0.0;
        }
    }
};  // class ZenohExpressionSubscriber


template <typename SchemaT>
class ZenohTypedSubscriber
{
  public:
    using Reader = typename SchemaT::Reader;

    ZenohTypedSubscriber(const std::string& zenoh_key,
                         std::function<void(Reader)> on_message) :
        zenoh_key_{zenoh_key},
        zenoh_session_{zenoh_session_manager::SessionManager::getOrCreate()},
        zenoh_subscriber_{}
    {
        if (!zenoh_session_)
        {
            SPDLOG_ERROR("No zenoh session available to subscribe to '{}'", zenoh_key_);
            return;
        }

        auto key_expr = zenoh::KeyExpr(zenoh_key_);
        zenoh_subscriber_ = std::make_unique<zenoh::Subscriber<void>>(
            zenoh_session_->declare_subscriber(
                key_expr,
                [cb = std::move(on_message)](const zenoh::Sample& sample)
                {
                    try
                    {
                        const std::vector<uint8_t> bytes = sample.get_payload().as_vector();
                        if (bytes.size() % sizeof(capnp::word) != 0)
                        {
                            SPDLOG_WARN("Typed subscriber received misaligned payload: {} bytes", bytes.size());
                            return;
                        }

                        auto words = kj::arrayPtr(reinterpret_cast<const capnp::word*>(bytes.data()), bytes.size() / sizeof(capnp::word));
                        capnp::FlatArrayMessageReader reader(words);
                        auto root = reader.getRoot<SchemaT>();
                        cb(root);
                    }
                    catch (const std::exception& e)
                    {
                        SPDLOG_ERROR("Typed subscriber handler error: {}", e.what());
                    }
                },
                zenoh::closures::none));

        SPDLOG_DEBUG("Typed subscriber active on '{}' for schema '{}'", zenoh_key_, schema_traits<SchemaT>::name);
    }

  private:
    std::string zenoh_key_;
    std::shared_ptr<zenoh::Session> zenoh_session_;
    std::unique_ptr<zenoh::Subscriber<void>> zenoh_subscriber_;
};

} // namespace pub_sub

#endif // ZENOH_SUBSCRIBER_H_