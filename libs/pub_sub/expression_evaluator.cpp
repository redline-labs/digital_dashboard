#include "pub_sub/expression_evaluator.h"

#include "helpers/unit_conversion.h"
#include "pub_sub/capnp_encoding.h"
#include "pub_sub/capnp_payload.h"
#include "reflection/reflection.h"

#include <capnp/dynamic.h>
#include <capnp/schema.h>
#include <capnp/serialize.h>

#define exprtk_disable_caseinsensitivity
#include <exprtk.hpp>

#include <spdlog/spdlog.h>

#include <cmath>
#include <map>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pub_sub
{

namespace
{

// Field names of a struct schema, for validating the expression's variables.
std::unordered_set<std::string> schemaFieldNames(const capnp::Schema& schema)
{
    std::unordered_set<std::string> names;
    if (schema.getProto().isStruct())
    {
        for (auto field : schema.asStruct().getFields())
        {
            names.insert(field.getProto().getName().cStr());
        }
    }
    return names;
}

}  // namespace

struct ExpressionEvaluator::Impl
{
    // Pre-computed field access, so a sample costs no schema lookups.
    struct FieldCache
    {
        capnp::StructSchema::Field field;
        capnp::DynamicValue::Type expected_type;

        // Where extractFieldValues() writes this field's value: straight into the
        // `variables` node that exprtk's symbol table is bound to.
        //
        // This used to be `variables[name] = value`, which walked a
        // std::map<std::string, double> and string-compared its way down the tree
        // once per field per sample -- about 40 of the 95 ns a sample cost, in a
        // cache built expressly to avoid per-sample lookups. std::map never
        // invalidates references to its nodes, and symbol_table.add_variable()
        // binds to this same address, so the pointer is stable for our lifetime.
        double* slot;
    };

    schema_type_t schema_type;
    std::string expression;
    std::string log_context;
    capnp::Schema schema{};
    bool is_valid = false;

    // Bound by address into symbol_table; see FieldCache::slot.
    std::map<std::string, double> variables;

    // The same names as `variables`, as a flat list for variableNames(). Built
    // once, because handing out a vector built on demand would mean returning a
    // reference to a temporary.
    std::vector<std::string> variable_names;

    exprtk::symbol_table<double> symbol_table;
    exprtk::expression<double> compiled_expression;
    exprtk::parser<double> parser;

    std::vector<FieldCache> field_cache;

    // schema_type comes from config -- it is what this consumer *expects* on this
    // key, not what is actually being published there. The publisher stamps the
    // truth on every sample, so check the two agree. Latched so a mismatch is
    // reported once rather than at the sample rate.
    bool schema_checked = false;

    // Latches for the other once-per-binding complaints. A malformed publisher
    // produces bad samples at the sample rate, and an unlatched warning at
    // 100 Hz churns the rotating log files and costs real CPU.
    bool payload_size_warned = false;
    bool non_finite_warned = false;
    bool range_warned = false;

    void extractFieldValues(capnp::DynamicStruct::Reader reader)
    {
        for (const auto& cached : field_cache)
        {
            auto value = reader.get(cached.field);
            double numeric = 0.0;

            switch (cached.expected_type)
            {
                case capnp::DynamicValue::BOOL:
                    numeric = value.as<bool>() ? 1.0 : 0.0;
                    break;

                case capnp::DynamicValue::INT:
                    numeric = static_cast<double>(value.as<int64_t>());
                    break;

                case capnp::DynamicValue::UINT:
                    numeric = static_cast<double>(value.as<uint64_t>());
                    break;

                case capnp::DynamicValue::FLOAT:
                    numeric = static_cast<double>(value.as<double>());
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
                    // Unreachable: buildFieldCache() rejects non-numeric fields up
                    // front, so an evaluator carrying one is never valid enough to
                    // reach evaluation. Kept silent -- this used to log per sample.
                    numeric = 0.0;
            }

            *cached.slot = numeric;
        }
    }

    void extractVariables()
    {
        std::vector<std::string> variable_list;
        if (exprtk::collect_variables(expression, symbol_table, variable_list))
        {
            for (const auto& var : variable_list)
            {
                variables[var] = 0.0;
            }
        }
        else
        {
            SPDLOG_ERROR("Failed to extract variables from expression '{}'", expression);
            is_valid = false;
            variables.clear();
        }
    }

    void validateVariablesAgainstSchema()
    {
        const auto fields = schemaFieldNames(schema);
        for (const auto& [var, unused] : variables)
        {
            if (fields.find(var) == fields.end())
            {
                SPDLOG_ERROR("Variable '{}' not found in schema '{}'", var,
                             reflection::enum_traits<pub_sub::schema_type_t>::to_string(
                                 schema_type));
                is_valid = false;
            }
        }
    }

    void buildFieldCache()
    {
        field_cache.clear();

        if (!schema.getProto().isStruct())
        {
            SPDLOG_ERROR("Schema is not a struct, cannot build field cache");
            return;
        }

        auto struct_schema = schema.asStruct();

        for (const auto& [var_name, unused] : variables)
        {
            auto field = struct_schema.getFieldByName(var_name.c_str());

            capnp::DynamicValue::Type expected_type = capnp::DynamicValue::UNKNOWN;
            auto field_type = field.getType();

            if (field_type.isBool())
            {
                expected_type = capnp::DynamicValue::BOOL;
            }
            else if (field_type.isInt8() || field_type.isInt16() || field_type.isInt32() ||
                     field_type.isInt64())
            {
                expected_type = capnp::DynamicValue::INT;
            }
            else if (field_type.isUInt8() || field_type.isUInt16() || field_type.isUInt32() ||
                     field_type.isUInt64())
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

            // A field the expression can never turn into a number is a config
            // error, and it is knowable right here rather than once per sample
            // forever. Previously this warned from extractFieldValues at the sample
            // rate and substituted 0.0, so the gauge read a confident, permanent
            // zero while the log filled up. Treat it like an unknown variable.
            // An if-chain rather than a switch: -Wswitch-enum makes a switch over a
            // large external enum like DynamicValue::Type impractical here.
            const bool is_numeric = (expected_type == capnp::DynamicValue::BOOL) ||
                                    (expected_type == capnp::DynamicValue::INT) ||
                                    (expected_type == capnp::DynamicValue::UINT) ||
                                    (expected_type == capnp::DynamicValue::FLOAT);
            if (!is_numeric)
            {
                SPDLOG_ERROR("Field '{}' of schema '{}' is not numeric, so expression '{}' cannot "
                             "be evaluated against it.",
                             var_name, reflection::enum_to_string(schema_type), expression);
                is_valid = false;
            }

            // `variables` was filled by extractVariables() before we got here, so
            // the entry exists and taking its address is safe; add_variable() binds
            // exprtk to the same one.
            field_cache.push_back({field, expected_type, &variables.at(var_name)});
        }
    }
};

ExpressionEvaluator::ExpressionEvaluator(schema_type_t schema_type,
                                         const std::string& expression,
                                         std::string log_context) :
    impl_(std::make_unique<Impl>())
{
    impl_->schema_type = schema_type;
    impl_->expression = expression;
    impl_->log_context = log_context.empty() ? expression : std::move(log_context);

    // Assume valid until proven otherwise.
    impl_->is_valid = true;

    if (impl_->expression.empty())
    {
        SPDLOG_ERROR("Expression is empty (context '{}')", impl_->log_context);
        impl_->is_valid = false;
        return;
    }

    const auto schema = get_schema(schema_type);
    if (!schema)
    {
        SPDLOG_ERROR("Schema '{}' not found in registry",
                     reflection::enum_traits<pub_sub::schema_type_t>::to_string(schema_type));
        impl_->is_valid = false;
        return;
    }
    impl_->schema = *schema;

    impl_->symbol_table.add_function("mph_to_mps", &mph_to_mps<double>);
    impl_->symbol_table.add_function("mps_to_mph", &mps_to_mph<double>);
    impl_->symbol_table.add_function("psi_to_bar", &psi_to_bar<double>);
    impl_->symbol_table.add_function("bar_to_psi", &bar_to_psi<double>);
    impl_->symbol_table.add_function("celsius_to_fahrenheit", &celsius_to_fahrenheit<double>);
    impl_->symbol_table.add_function("fahrenheit_to_celsius", &fahrenheit_to_celsius<double>);

    impl_->extractVariables();
    impl_->validateVariablesAgainstSchema();

    if (impl_->is_valid)
    {
        impl_->buildFieldCache();

        for (auto& [var, value] : impl_->variables)
        {
            impl_->symbol_table.add_variable(var, value);
        }
        impl_->symbol_table.add_constants();

        impl_->compiled_expression.register_symbol_table(impl_->symbol_table);
        impl_->is_valid = impl_->parser.compile(impl_->expression, impl_->compiled_expression);
    }

    // std::map iterates in sorted key order, which is where variableNames()'s
    // ordering guarantee comes from.
    impl_->variable_names.reserve(impl_->variables.size());
    for (const auto& [var, unused] : impl_->variables)
    {
        impl_->variable_names.push_back(var);
    }
}

ExpressionEvaluator::~ExpressionEvaluator() = default;

bool ExpressionEvaluator::isValid() const
{
    return impl_->is_valid;
}

schema_type_t ExpressionEvaluator::getSchemaType() const
{
    return impl_->schema_type;
}

const std::string& ExpressionEvaluator::getExpression() const
{
    return impl_->expression;
}

const std::vector<std::string>& ExpressionEvaluator::variableNames() const
{
    return impl_->variable_names;
}

void ExpressionEvaluator::checkPublishedSchema(std::string_view encoding)
{
    if (impl_->schema_checked)
    {
        return;
    }
    impl_->schema_checked = true;

    const std::string_view published = schemaNameFromEncoding(encoding);
    const std::string_view configured =
        reflection::enum_traits<pub_sub::schema_type_t>::to_string(impl_->schema_type);

    if (published == configured)
    {
        return;
    }

    // An empty schema half means the publisher set a MIME type but no schema
    // (or is not one of ours at all). Not necessarily wrong, so say less.
    if (published.empty() || published == encoding)
    {
        SPDLOG_DEBUG("Key '{}' carries encoding '{}', which names no schema; "
                     "decoding as the configured '{}'",
                     impl_->log_context, encoding, configured);
        return;
    }

    // capnp will decode the payload against whatever schema it is handed --
    // field offsets simply land on different bytes -- so a wrong schema in
    // config produces a plausible but meaningless number rather than an error.
    // This is the only place that mismatch is detectable.
    SPDLOG_ERROR("Key '{}' is published as '{}' but is configured as '{}'. The value will be "
                 "decoded against the configured schema and will be wrong; fix schema_type in "
                 "the config.",
                 impl_->log_context, published, configured);
}

std::optional<double> ExpressionEvaluator::evaluateToDouble(
    const std::vector<std::uint8_t>& payload)
{
    if (!impl_->is_valid)
    {
        SPDLOG_ERROR("Expression is not valid, cannot evaluate.");
        return std::nullopt;
    }

    // capnp reads whole 8-byte words. A payload that is not a multiple of
    // sizeof(word) used to be silently truncated, and anything under one word
    // decoded as an empty message -- every field its default, no warning, a gauge
    // reading zero. Say so instead.
    const WordAlignedPayload aligned(payload);
    if (aligned.empty())
    {
        if (!impl_->payload_size_warned)
        {
            impl_->payload_size_warned = true;
            SPDLOG_WARN("Key '{}': payload of {} bytes is not a whole number of {}-byte capnp "
                        "words; ignoring these samples (further occurrences not logged).",
                        impl_->log_context, payload.size(), sizeof(capnp::word));
        }
        return std::nullopt;
    }

    try
    {
        capnp::FlatArrayMessageReader message_reader(aligned.words());
        impl_->extractFieldValues(
            message_reader.getRoot<capnp::DynamicStruct>(impl_->schema.asStruct()));

        const double result = impl_->compiled_expression.value();

        // exprtk does plain IEEE division, so `x/0` is inf and `0/0` is NaN, with
        // no throw and no flag. Both then poison whatever they touch:
        // static_cast<int>(NaN) is undefined behaviour, and std::clamp passes NaN
        // straight through to painter.rotate(). Stop it at the boundary.
        if (!std::isfinite(result))
        {
            if (!impl_->non_finite_warned)
            {
                impl_->non_finite_warned = true;
                SPDLOG_WARN("Key '{}': expression '{}' evaluated to {}; ignoring these samples "
                            "(further occurrences not logged).",
                            impl_->log_context, impl_->expression, result);
            }
            return std::nullopt;
        }

        return result;
    }
    catch (const std::exception& e)
    {
        SPDLOG_ERROR("Expression evaluation failed: {}", e.what());
        return std::nullopt;
    }
}

void ExpressionEvaluator::warnOutOfRange(double value)
{
    if (impl_->range_warned)
    {
        return;
    }
    impl_->range_warned = true;
    SPDLOG_WARN("Key '{}': expression '{}' produced {}, which does not fit the configured type; "
                "ignoring these samples (further occurrences not logged).",
                impl_->log_context, impl_->expression, value);
}

}  // namespace pub_sub
