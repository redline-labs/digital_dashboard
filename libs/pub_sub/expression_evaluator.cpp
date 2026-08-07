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
    // THE AWKWARD PART: exprtk fixes a vector's length when it is registered and
    // range-checks every literal index against it AT COMPILE TIME, while a capnp
    // list's length is a property of each message and is unknown until one
    // arrives. So the length is discovered from the first sample and the
    // expression is recompiled against it -- see resizeVectors().
    //
    // The alternative was to register some generous capacity and zero-fill the
    // tail. It is simpler and it is wrong in two ways at once: an index past the
    // real end reads a confident permanent zero, which is the exact failure this
    // evaluator already refuses to make for a non-numeric field, and `avg()`
    // divides by the capacity rather than by the length, so the answer is quietly
    // scaled by 32/64 with nothing to say so.
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

        // What the schema says the length always is, when it says. A message
        // that contradicts it is a PUBLISHER bug rather than something to work
        // around silently, so it is reported once.
        std::optional<std::uint32_t> declared_length;
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

    bool list_index_warned = false;
    bool length_mismatch_warned = false;

    // What a vector is registered with before any message has been seen. Large
    // enough that a plausible index compiles (the PDM has 32 outputs, CAN FD
    // carries 64 bytes) and never used to READ anything: the first sample
    // replaces it with the true length.
    static constexpr std::size_t kProvisionalListLength = 64;

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

    // Point exprtk at vectors of the lengths this message actually carries, and
    // recompile against them.
    //
    // Called only when a length CHANGES, which in practice is once: the first
    // sample. A publisher whose list length varies per message pays a recompile
    // each time it changes, which is the honest cost of an expression whose valid
    // indices vary per message.
    //
    // False means the expression cannot be evaluated against these lengths --
    // an index the real stream does not reach. That is reported once and then
    // every sample is dropped, rather than answering with a zero.
    bool resizeVectors(const std::vector<std::size_t>& lengths)
    {
        // RELEASE THE COMPILED TREE FIRST, before the buffers it points into are
        // touched. exprtk's compiled expression holds the vector's data pointer
        // and length, and std::vector::assign to a different size is free to
        // reallocate -- so mutating the vectors while the old tree is still alive
        // leaves it holding a dangling pointer, and destroying it afterwards is a
        // use-after-free. It presented as an INTERMITTENT segfault in the
        // evaluator's own test, roughly one run in three, because whether the
        // buffer actually moved depended on the allocator.
        compiled_expression = exprtk::expression<double>();

        for (std::size_t i = 0; i < list_cache.size(); ++i)
        {
            const ListCache& cached = list_cache[i];
            symbol_table.remove_vector(cached.name);
            cached.data->assign(lengths[i], 0.0);

            // A zero-length list cannot be registered with exprtk and cannot
            // satisfy any index, so it is the same answer as an out-of-range one.
            if (cached.data->empty() ||
                !symbol_table.add_vector(cached.name, cached.data->data(), cached.data->size()))
            {
                reportListProblem(cached.name, lengths[i]);
                return false;
            }
        }

        compiled_expression.register_symbol_table(symbol_table);

        if (!parser.compile(expression, compiled_expression))
        {
            // The index is out of range for the length the stream really carries.
            // exprtk's own message names the vector and both numbers.
            if (!list_index_warned)
            {
                list_index_warned = true;
                SPDLOG_ERROR("Key '{}': expression '{}' cannot be evaluated against the list "
                             "lengths this stream actually carries: {}. Samples are being "
                             "dropped (further occurrences not logged).",
                             log_context, expression, parser.error());
            }
            return false;
        }

        return true;
    }

    void reportListProblem(const std::string& name, std::size_t length)
    {
        if (list_index_warned)
        {
            return;
        }
        list_index_warned = true;
        SPDLOG_ERROR("Key '{}': list '{}' arrived with {} element(s), which expression '{}' "
                     "cannot be evaluated against. Samples are being dropped (further "
                     "occurrences not logged).",
                     log_context, name, length, expression);
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

        // Resize before reading, and only when something moved: the compare is
        // one integer per list per sample, against a recompile that essentially
        // never happens after the first message.
        // "Has it changed" alone, with no separate first-sample flag: the vectors
        // start at kProvisionalListLength, so the first message differs from it
        // unless it happens to carry exactly that many elements -- in which case
        // the provisional registration was already correct and there is nothing
        // to do. A `lists_sized` flag alongside this looked like it was carrying
        // the first-sample case and was carrying nothing; removing it is the
        // honest version, and the tests do not notice, which is the proof.
        std::vector<std::size_t> lengths;
        lengths.reserve(list_cache.size());
        bool changed = false;
        for (const ListCache& cached : list_cache)
        {
            const std::size_t length = reader.get(cached.field).as<capnp::DynamicList>().size();

            // The schema made a claim; this message either honours it or does
            // not. Saying so once beats either trusting the annotation over the
            // wire or quietly preferring the wire over the annotation.
            if (cached.declared_length && *cached.declared_length != length &&
                !length_mismatch_warned)
            {
                length_mismatch_warned = true;
                SPDLOG_WARN("Key '{}': list '{}' is declared as {} element(s) but this message "
                            "carries {}. Using what arrived (further occurrences not logged).",
                            log_context, cached.name, *cached.declared_length, length);
            }

            lengths.push_back(length);
            changed = changed || (length != cached.data->size());
        }

        if (changed)
        {
            if (!resizeVectors(lengths))
            {
                return false;
            }
        }

        for (const ListCache& cached : list_cache)
        {
            auto list = reader.get(cached.field).as<capnp::DynamicList>();
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

                // REGISTERED AT THE DECLARED LENGTH WHEN THE SCHEMA GIVES ONE,
                // because exprtk range-checks every literal index against the
                // registered size AT COMPILE TIME. So `values[40]` against a
                // thirty-two output PDM stops being a binding that succeeds and
                // then drops every sample, and becomes a construction error --
                // which is where this evaluator refuses everything else it can
                // know up front.
                //
                // Without an annotation there is nothing to check against until
                // a message arrives, so it falls back to the provisional length
                // and the first sample sizes it. See schemas/annotations.capnp.
                const auto declared = fixedListLength(field);
                vectors[var_name].assign(declared.value_or(kProvisionalListLength), 0.0);
                list_cache.push_back(
                    {field, element_type, var_name, &vectors.at(var_name), declared});
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

        // Provisionally sized -- see Impl::kProvisionalListLength. Compiling
        // against it proves the expression PARSES and catches an index no
        // plausible stream could satisfy; whether the real stream reaches that
        // index is not knowable until a message arrives, and is reported then.
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
