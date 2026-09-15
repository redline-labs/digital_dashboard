// SPDX-License-Identifier: GPL-3.0-or-later
//
// Every list the PDM node publishes, against the length its schema declares.
//
// A consumer compiles an expression against the $fixedLength in the schema and
// drops any message whose list is a different length, whole and without a
// value. So a node that publishes one element too many is not slightly wrong:
// every expression over that field reads nothing, forever, and the only trace is
// one log line. The node did exactly that with `inputs` (24 against 23). The
// last check drives the real evaluator over a filled message, which is the
// failure a user saw.

#include "pdm_messages.h"

#include "pub_sub/capnp_json.h"
#include "pub_sub/expression_evaluator.h"
#include "pub_sub/schema_registry.h"

#include <capnp/message.h>
#include <capnp/serialize.h>
#include <spdlog/spdlog.h>

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace
{

int failures = 0;

void expect(bool condition, const std::string& what)
{
    if (!condition)
    {
        ++failures;
        SPDLOG_ERROR("FAIL: {}", what);
    }
}

std::optional<std::uint32_t> declaredLength(pub_sub::schema_type_t type, const char* field_name)
{
    const auto schema = pub_sub::get_schema(type);
    if (!schema)
    {
        return std::nullopt;
    }
    for (auto field : schema->asStruct().getFields())
    {
        if (std::string(field.getProto().getName().cStr()) == field_name)
        {
            return pub_sub::fixedListLength(field);
        }
    }
    return std::nullopt;
}

std::vector<std::uint8_t> toBytes(capnp::MallocMessageBuilder& message)
{
    const kj::Array<capnp::word> words = capnp::messageToFlatArray(message);
    const kj::ArrayPtr<const kj::byte> bytes = words.asBytes();
    return {bytes.begin(), bytes.end()};
}

// One schema's list, filled from a default-constructed DBC message, checked
// against the schema's declared length.
template <typename Schema, typename Dbc, typename Fill, typename GetList>
void checkLength(const char* what, pub_sub::schema_type_t type, const char* field, Fill fill,
                 GetList get)
{
    const auto declared = declaredLength(type, field);
    expect(declared.has_value(), std::string(what) + ": the schema declares a length");

    capnp::MallocMessageBuilder message;
    auto root = message.initRoot<Schema>();
    fill(Dbc{}, root);
    const auto published = get(root.asReader()).size();
    expect(declared && published == *declared,
           std::string(what) + ": publishes " + std::to_string(published) + " element(s), schema says " +
               (declared ? std::to_string(*declared) : std::string("nothing")));

    // The builder is reused across messages; a second fill must not grow it.
    fill(Dbc{}, root);
    expect(get(root.asReader()).size() == published, std::string(what) + ": a refill keeps the length");
}

void testEveryListMatchesItsSchema()
{
    using namespace motec_pdm;
    using T = pub_sub::schema_type_t;

    checkLength<MotecPdmInputState, PDM_Input_State_0x500_t>(
        "input_state.inputs", T::MotecPdmInputState, "inputs", fillInputState,
        [](MotecPdmInputState::Reader r) { return r.getInputs(); });
    checkLength<MotecPdmInputVoltage, PDM_Input_Voltage_0x505_t>(
        "input_voltage.values", T::MotecPdmInputVoltage, "values", fillInputVoltage,
        [](MotecPdmInputVoltage::Reader r) { return r.getValues(); });
    checkLength<MotecPdmOutputCurrent, PDM_Output_Current_0x501_t>(
        "output_current.values", T::MotecPdmOutputCurrent, "values", fillOutputCurrent,
        [](MotecPdmOutputCurrent::Reader r) { return r.getValues(); });
    checkLength<MotecPdmOutputLoad, PDM_Output_Load_0x502_t>(
        "output_load.values", T::MotecPdmOutputLoad, "values", fillOutputLoad,
        [](MotecPdmOutputLoad::Reader r) { return r.getValues(); });
    checkLength<MotecPdmOutputVoltage, PDM_Output_Voltage_0x503_t>(
        "output_voltage.values", T::MotecPdmOutputVoltage, "values", fillOutputVoltage,
        [](MotecPdmOutputVoltage::Reader r) { return r.getValues(); });
    checkLength<MotecPdmOutputStatus, PDM_Output_Status_0x504_t>(
        "output_status.values", T::MotecPdmOutputStatus, "values", fillOutputStatus,
        [](MotecPdmOutputStatus::Reader r) { return r.getValues(); });
}

// The last channel of each kind lands in the last slot, and nothing else moves.
void testLastChannelLandsInLastSlot()
{
    using namespace motec_pdm;

    {
        PDM_Input_State_0x500_t m{};
        m.PDM_Input_23 = 1;
        capnp::MallocMessageBuilder message;
        auto root = message.initRoot<MotecPdmInputState>();
        fillInputState(m, root);
        const auto inputs = root.asReader().getInputs();
        expect(inputs.size() == 23 && inputs[22], "PDM_Input_23 is inputs[22]");
        bool others = false;
        for (unsigned i = 0; i + 1 < inputs.size(); ++i)
        {
            others = others || inputs[i];
        }
        expect(!others, "and no other input is set");
    }
    {
        PDM_Input_Voltage_0x505_t m{};
        m.PDM_Input_Voltage_23 = 12.4f;
        capnp::MallocMessageBuilder message;
        auto root = message.initRoot<MotecPdmInputVoltage>();
        fillInputVoltage(m, root);
        const auto values = root.asReader().getValues();
        expect(values.size() == 23 && std::abs(values[22] - 12.4f) < 1e-4f,
               "PDM_Input_Voltage_23 is values[22]");
    }
    {
        PDM_Output_Current_0x501_t m{};
        m.PDM_Output_Current_32 = 7.5;
        capnp::MallocMessageBuilder message;
        auto root = message.initRoot<MotecPdmOutputCurrent>();
        fillOutputCurrent(m, root);
        const auto values = root.asReader().getValues();
        expect(values.size() == 32 && std::abs(values[31] - 7.5f) < 1e-4f,
               "PDM_Output_Current_32 is values[31]");
    }
    {
        PDM_Output_Status_0x504_t m{};
        m.PDM_Output_Status_32 = decltype(m.PDM_Output_Status_32)::Output_Over_Current_Error;
        capnp::MallocMessageBuilder message;
        auto root = message.initRoot<MotecPdmOutputStatus>();
        fillOutputStatus(m, root);
        const auto values = root.asReader().getValues();
        expect(values.size() == 32 && values[31] == PdmOutputStatusEnum::OVER_CURRENT_ERROR,
               "PDM_Output_Status_32 is values[31]");
    }
}

// What a dashboard or scope actually does with the topic.
void testAnExpressionReadsTheLastInput()
{
    using namespace motec_pdm;
    PDM_Input_State_0x500_t m{};
    m.PDM_Input_23 = 1;
    capnp::MallocMessageBuilder message;
    motec_pdm::fillInputState(m, message.initRoot<MotecPdmInputState>());
    const auto payload = toBytes(message);

    pub_sub::ExpressionEvaluator eval(pub_sub::schema_type_t::MotecPdmInputState, "inputs[22]",
                                      "nodes/motec_pdm/input_state");
    expect(eval.isValid(), "inputs[22] compiles against the input_state schema");
    const auto value = eval.evaluate<double>(payload);
    expect(value.has_value(), "a message the node published produces a reading");
    expect(value.has_value() && *value == 1.0, "and the reading is the last input's state");
}

}  // namespace

int main()
{
    spdlog::set_pattern("[%^%l%$] %v");

    testEveryListMatchesItsSchema();
    testLastChannelLandsInLastSlot();
    testAnExpressionReadsTheLastInput();

    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }
    SPDLOG_INFO("all checks passed");
    return 0;
}
