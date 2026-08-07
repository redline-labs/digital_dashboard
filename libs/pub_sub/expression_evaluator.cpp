#include "pub_sub/expression_evaluator.h"

#include "helpers/unit_conversion.h"
#include "pub_sub/capnp_encoding.h"
#include "pub_sub/capnp_json.h"
#include "pub_sub/capnp_payload.h"
#include "reflection/reflection.h"

#include <capnp/dynamic.h>
#include <capnp/schema.h>
#include <capnp/serialize.h>

#define exprtk_disable_caseinsensitivity
#include <exprtk.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
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

// What a capnp type decodes to, or UNKNOWN for one an expression can never turn
// into a number.
//
// ENUM counts, and reads as its ORDINAL -- the position in the enum's
// declaration, which is what capnp stores. Not the name, obviously, and not the
// value any comment in the .capnp file mentions: MotecPdm's status enumerants
// are annotated with the DBC's 0/1/2/4/8 bitmask, but capnp stores 0..4 and the
// node does that translation on the way in. A consumer wanting the NAME reads
// the enumerant list out of describeSchema(), which has always carried it.
capnp::DynamicValue::Type numericTypeOf(const capnp::Type& type)
{
    if (type.isBool())
    {
        return capnp::DynamicValue::BOOL;
    }
    if (type.isInt8() || type.isInt16() || type.isInt32() || type.isInt64())
    {
        return capnp::DynamicValue::INT;
    }
    if (type.isUInt8() || type.isUInt16() || type.isUInt32() || type.isUInt64())
    {
        return capnp::DynamicValue::UINT;
    }
    if (type.isFloat32() || type.isFloat64())
    {
        return capnp::DynamicValue::FLOAT;
    }
    if (type.isEnum())
    {
        return capnp::DynamicValue::ENUM;
    }
    return capnp::DynamicValue::UNKNOWN;
}

// One decoded value as a double. An if-chain rather than a switch for the reason
// the old code gave: -Wswitch-enum over DynamicValue::Type, an external enum with
// a dozen alternatives we do not care about, is noise rather than safety.
double numericFrom(const capnp::DynamicValue::Reader& value, capnp::DynamicValue::Type as)
{
    if (as == capnp::DynamicValue::BOOL)
    {
        return value.as<bool>() ? 1.0 : 0.0;
    }
    if (as == capnp::DynamicValue::INT)
    {
        return static_cast<double>(value.as<int64_t>());
    }
    if (as == capnp::DynamicValue::UINT)
    {
        return static_cast<double>(value.as<uint64_t>());
    }
    if (as == capnp::DynamicValue::FLOAT)
    {
        return value.as<double>();
    }
    if (as == capnp::DynamicValue::ENUM)
    {
        return static_cast<double>(value.as<capnp::DynamicEnum>().getRaw());
    }

    // Unreachable: buildFieldCache() rejects anything else up front, so an
    // evaluator carrying one is never valid enough to reach evaluation.
    return 0.0;
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

    // A list field, bound to exprtk as a VECTOR so `values[7]` is ordinary
    // syntax and `sum(values)`, `max(values)`, `avg(values)` come for free --
    // and total PDM current really is `sum(values)` over its 32 outputs.
    //
    // THE LENGTH COMES FROM THE SCHEMA, via $fixedLength. exprtk fixes a
    // vector's length when it is registered and range-checks every literal
    // index against it at compile time, and capnp declares no length at all --
    // so without the annotation there is nothing to compile against. Requiring
    // it means the expression is compiled exactly once, every index is checked
    // against the real count before any message arrives, and sum()/avg() divide
    // by the right number by construction.
    //
    struct ListCache
    {
        capnp::StructSchema::Field field;

        // What the ELEMENTS decode as. The list itself is not one of these.
        capnp::DynamicValue::Type element_type;

        std::string name;

        // Into `vectors`, whose nodes std::map never moves -- the same stability
        // argument FieldCache::slot relies on. add_vector() binds exprtk to
        // data(), so this is re-registered whenever the vector is resized.
        std::vector<double>* data;

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

    // Bound by address into symbol_table as exprtk vectors; see ListCache.
    std::map<std::string, std::vector<double>> vectors;
    std::vector<ListCache> list_cache;

    bool length_mismatch_warned = false;

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

    void reportLengthMismatch(const std::string& name, std::size_t declared, std::size_t actual)
    {
        if (length_mismatch_warned)
        {
            return;
        }
        length_mismatch_warned = true;
        SPDLOG_ERROR("Key '{}': list '{}' is declared as {} element(s) but this message carries "
                     "{}. Skipping these samples -- the expression was compiled against the "
                     "declared length and the indices in it may not exist here (further "
                     "occurrences not logged).",
                     log_context, name, declared, actual);
    }

    // False when this sample cannot be turned into values, which the caller
    // reports as "no reading" rather than as a number.
    bool extractFieldValues(capnp::DynamicStruct::Reader reader)
    {
        for (const auto& cached : field_cache)
        {
            *cached.slot = numericFrom(reader.get(cached.field), cached.expected_type);
        }

        if (list_cache.empty())
        {
            return true;
        }

        for (const ListCache& cached : list_cache)
        {
            auto list = reader.get(cached.field).as<capnp::DynamicList>();

            // THE LENGTH IS SETTLED AT CONSTRUCTION, so this is a check rather
            // than a resize. The expression was compiled against the declared
            // length and every literal index in it was range-checked against
            // that -- a message carrying a different count may simply not have
            // the elements the expression names, so the honest answer is no
            // reading rather than a number read from somewhere else.
            //
            // This is also what keeps a variable-length list from recompiling
            // the expression per message. Nothing here allocates or compiles.
            if (list.size() != cached.data->size())
            {
                reportLengthMismatch(cached.name, cached.data->size(), list.size());
                return false;
            }

            for (std::size_t i = 0; i < cached.data->size(); ++i)
            {
                (*cached.data)[i] = numericFrom(list[static_cast<unsigned>(i)],
                                                cached.element_type);
            }
        }

        return true;
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
        list_cache.clear();

        if (!schema.getProto().isStruct())
        {
            SPDLOG_ERROR("Schema is not a struct, cannot build field cache");
            return;
        }

        auto struct_schema = schema.asStruct();

        // Names that turn out to be lists move out of `variables` and into
        // `vectors`, so collect them first rather than mutating while iterating.
        std::vector<std::string> list_names;

        for (const auto& [var_name, unused] : variables)
        {
            auto field = struct_schema.getFieldByName(var_name.c_str());
            const auto field_type = field.getType();

            if (field_type.isList())
            {
                const auto element_type =
                    numericTypeOf(field_type.asList().getElementType());

                // A List(Text) or a List(SomeStruct) is no more a number than a
                // bare Text field is, and saying so here rather than per sample is
                // the same argument the scalar case makes below.
                if (element_type == capnp::DynamicValue::UNKNOWN)
                {
                    SPDLOG_ERROR("Field '{}' of schema '{}' is a list whose elements are not "
                                 "numeric, so expression '{}' cannot be evaluated against it.",
                                 var_name, reflection::enum_to_string(schema_type), expression);
                    is_valid = false;
                    continue;
                }

                list_names.push_back(var_name);

                // A LIST MUST DECLARE ITS LENGTH TO BE BINDABLE.
                //
                // capnp gives no length, so without the annotation there is
                // nothing to compile an index against and nothing a picker can
                // offer. The alternative -- discover the length from the first
                // message and recompile whenever it changes -- worked, and its
                // cost was a recompile per message for a list that genuinely
                // varies. CanFrame.data is List(UInt8) of 1..64 bytes on the
                // highest-rate topic in the tree, so that cost lands exactly
                // where it can least be afforded.
                //
                // Requiring the annotation makes the length a fact known at
                // construction: the expression compiles once, exprtk
                // range-checks every literal index against the real count, and
                // sum()/avg() are correct by construction. See
                // schemas/annotations.capnp.
                const auto declared = fixedListLength(field);
                if (!declared)
                {
                    SPDLOG_ERROR("Field '{}' of schema '{}' is a list with no declared length, so "
                                 "expression '{}' cannot be compiled against it. Annotate the "
                                 "field with $fixedLength(N) if its count is fixed; a genuinely "
                                 "variable list is not plottable element by element.",
                                 var_name, reflection::enum_to_string(schema_type), expression);
                    is_valid = false;
                    continue;
                }

                list_names.push_back(var_name);
                vectors[var_name].assign(*declared, 0.0);
                list_cache.push_back({field, element_type, var_name, &vectors.at(var_name)});
                continue;
            }

            const capnp::DynamicValue::Type expected_type = numericTypeOf(field_type);

            // A field the expression can never turn into a number is a config
            // error, and it is knowable right here rather than once per sample
            // forever. Previously this warned from extractFieldValues at the sample
            // rate and substituted 0.0, so the gauge read a confident, permanent
            // zero while the log filled up. Treat it like an unknown variable.
            if (expected_type == capnp::DynamicValue::UNKNOWN)
            {
                SPDLOG_ERROR("Field '{}' of schema '{}' is not numeric, so expression '{}' cannot "
                             "be evaluated against it.",
                             var_name, reflection::enum_to_string(schema_type), expression);
                is_valid = false;
                continue;
            }

            // `variables` was filled by extractVariables() before we got here, so
            // the entry exists and taking its address is safe; add_variable() binds
            // exprtk to the same one.
            field_cache.push_back({field, expected_type, &variables.at(var_name)});
        }

        // A list is bound as a vector, so it must NOT also be registered as a
        // scalar of the same name -- exprtk would take the first registration and
        // `values[3]` would fail to parse against a plain double.
        for (const std::string& name : list_names)
        {
            variables.erase(name);
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

        // Sized from the schema, so this compile is the only one: exprtk
        // range-checks every literal index against the declared count, and an
        // index the stream cannot reach is a construction error rather than a
        // binding that quietly drops every sample.
        for (auto& [var, data] : impl_->vectors)
        {
            impl_->symbol_table.add_vector(var, data.data(), data.size());
        }

        impl_->symbol_table.add_constants();

        impl_->compiled_expression.register_symbol_table(impl_->symbol_table);
        impl_->is_valid = impl_->parser.compile(impl_->expression, impl_->compiled_expression);
    }

    // Both halves: a list field is a field the expression reads, and
    // variableNames() promises the fields it reads. buildFieldCache() moves list
    // names out of `variables` and into `vectors` -- they must not be registered
    // twice with exprtk -- so reading only the first would silently under-report
    // exactly the bindings that are hardest to reason about.
    //
    // std::map iterates in sorted key order; merging two sorted ranges keeps the
    // ordering guarantee the header makes.
    impl_->variable_names.reserve(impl_->variables.size() + impl_->vectors.size());
    for (const auto& [var, unused] : impl_->variables)
    {
        impl_->variable_names.push_back(var);
    }
    for (const auto& [var, unused] : impl_->vectors)
    {
        impl_->variable_names.push_back(var);
    }
    std::sort(impl_->variable_names.begin(), impl_->variable_names.end());
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
    std::span<const std::uint8_t> payload)
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
    const WordAlignedPayload aligned(reinterpret_cast<const kj::byte*>(payload.data()),
                                     payload.size());
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
        if (!impl_->extractFieldValues(
                message_reader.getRoot<capnp::DynamicStruct>(impl_->schema.asStruct())))
        {
            // A list this expression indexes past the end of. Already reported
            // once, by name and length.
            return std::nullopt;
        }

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
