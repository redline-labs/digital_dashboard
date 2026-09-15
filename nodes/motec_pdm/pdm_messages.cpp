// SPDX-License-Identifier: GPL-3.0-or-later

#include "pdm_messages.h"

#include <array>
#include <cstdint>

namespace motec_pdm
{

namespace
{

// Sized by its initializer, so a missing or extra signal is a build error below
// rather than a null entry or a slot nobody writes. Accessors rather than member
// pointers because the generator types a signal by its scaling, and one PDM
// message mixes integer and floating-point signals.
const auto kInputFields = std::to_array<bool (*)(const PDM_Input_State_0x500_t&)>({
    +[](const PDM_Input_State_0x500_t& m) { return m.PDM_Input_1 != 0; },
    +[](const PDM_Input_State_0x500_t& m) { return m.PDM_Input_2 != 0; },
    +[](const PDM_Input_State_0x500_t& m) { return m.PDM_Input_3 != 0; },
    +[](const PDM_Input_State_0x500_t& m) { return m.PDM_Input_4 != 0; },
    +[](const PDM_Input_State_0x500_t& m) { return m.PDM_Input_5 != 0; },
    +[](const PDM_Input_State_0x500_t& m) { return m.PDM_Input_6 != 0; },
    +[](const PDM_Input_State_0x500_t& m) { return m.PDM_Input_7 != 0; },
    +[](const PDM_Input_State_0x500_t& m) { return m.PDM_Input_8 != 0; },
    +[](const PDM_Input_State_0x500_t& m) { return m.PDM_Input_9 != 0; },
    +[](const PDM_Input_State_0x500_t& m) { return m.PDM_Input_10 != 0; },
    +[](const PDM_Input_State_0x500_t& m) { return m.PDM_Input_11 != 0; },
    +[](const PDM_Input_State_0x500_t& m) { return m.PDM_Input_12 != 0; },
    +[](const PDM_Input_State_0x500_t& m) { return m.PDM_Input_13 != 0; },
    +[](const PDM_Input_State_0x500_t& m) { return m.PDM_Input_14 != 0; },
    +[](const PDM_Input_State_0x500_t& m) { return m.PDM_Input_15 != 0; },
    +[](const PDM_Input_State_0x500_t& m) { return m.PDM_Input_16 != 0; },
    +[](const PDM_Input_State_0x500_t& m) { return m.PDM_Input_17 != 0; },
    +[](const PDM_Input_State_0x500_t& m) { return m.PDM_Input_18 != 0; },
    +[](const PDM_Input_State_0x500_t& m) { return m.PDM_Input_19 != 0; },
    +[](const PDM_Input_State_0x500_t& m) { return m.PDM_Input_20 != 0; },
    +[](const PDM_Input_State_0x500_t& m) { return m.PDM_Input_21 != 0; },
    +[](const PDM_Input_State_0x500_t& m) { return m.PDM_Input_22 != 0; },
    +[](const PDM_Input_State_0x500_t& m) { return m.PDM_Input_23 != 0; },
});
static_assert(kInputFields.size() == kInputCount);

const auto kInputVoltageFields = std::to_array<float (*)(const PDM_Input_Voltage_0x505_t&)>({
    +[](const PDM_Input_Voltage_0x505_t& m) { return static_cast<float>(m.PDM_Input_Voltage_1); },
    +[](const PDM_Input_Voltage_0x505_t& m) { return static_cast<float>(m.PDM_Input_Voltage_2); },
    +[](const PDM_Input_Voltage_0x505_t& m) { return static_cast<float>(m.PDM_Input_Voltage_3); },
    +[](const PDM_Input_Voltage_0x505_t& m) { return static_cast<float>(m.PDM_Input_Voltage_4); },
    +[](const PDM_Input_Voltage_0x505_t& m) { return static_cast<float>(m.PDM_Input_Voltage_5); },
    +[](const PDM_Input_Voltage_0x505_t& m) { return static_cast<float>(m.PDM_Input_Voltage_6); },
    +[](const PDM_Input_Voltage_0x505_t& m) { return static_cast<float>(m.PDM_Input_Voltage_7); },
    +[](const PDM_Input_Voltage_0x505_t& m) { return static_cast<float>(m.PDM_Input_Voltage_8); },
    +[](const PDM_Input_Voltage_0x505_t& m) { return static_cast<float>(m.PDM_Input_Voltage_9); },
    +[](const PDM_Input_Voltage_0x505_t& m) { return static_cast<float>(m.PDM_Input_Voltage_10); },
    +[](const PDM_Input_Voltage_0x505_t& m) { return static_cast<float>(m.PDM_Input_Voltage_11); },
    +[](const PDM_Input_Voltage_0x505_t& m) { return static_cast<float>(m.PDM_Input_Voltage_12); },
    +[](const PDM_Input_Voltage_0x505_t& m) { return static_cast<float>(m.PDM_Input_Voltage_13); },
    +[](const PDM_Input_Voltage_0x505_t& m) { return static_cast<float>(m.PDM_Input_Voltage_14); },
    +[](const PDM_Input_Voltage_0x505_t& m) { return static_cast<float>(m.PDM_Input_Voltage_15); },
    +[](const PDM_Input_Voltage_0x505_t& m) { return static_cast<float>(m.PDM_Input_Voltage_16); },
    +[](const PDM_Input_Voltage_0x505_t& m) { return static_cast<float>(m.PDM_Input_Voltage_17); },
    +[](const PDM_Input_Voltage_0x505_t& m) { return static_cast<float>(m.PDM_Input_Voltage_18); },
    +[](const PDM_Input_Voltage_0x505_t& m) { return static_cast<float>(m.PDM_Input_Voltage_19); },
    +[](const PDM_Input_Voltage_0x505_t& m) { return static_cast<float>(m.PDM_Input_Voltage_20); },
    +[](const PDM_Input_Voltage_0x505_t& m) { return static_cast<float>(m.PDM_Input_Voltage_21); },
    +[](const PDM_Input_Voltage_0x505_t& m) { return static_cast<float>(m.PDM_Input_Voltage_22); },
    +[](const PDM_Input_Voltage_0x505_t& m) { return static_cast<float>(m.PDM_Input_Voltage_23); },
});
static_assert(kInputVoltageFields.size() == kInputCount);

const auto kOutputCurrentFields = std::to_array<float (*)(const PDM_Output_Current_0x501_t&)>({
    +[](const PDM_Output_Current_0x501_t& m) { return static_cast<float>(m.PDM_Output_Current_1); },
    +[](const PDM_Output_Current_0x501_t& m) { return static_cast<float>(m.PDM_Output_Current_2); },
    +[](const PDM_Output_Current_0x501_t& m) { return static_cast<float>(m.PDM_Output_Current_3); },
    +[](const PDM_Output_Current_0x501_t& m) { return static_cast<float>(m.PDM_Output_Current_4); },
    +[](const PDM_Output_Current_0x501_t& m) { return static_cast<float>(m.PDM_Output_Current_5); },
    +[](const PDM_Output_Current_0x501_t& m) { return static_cast<float>(m.PDM_Output_Current_6); },
    +[](const PDM_Output_Current_0x501_t& m) { return static_cast<float>(m.PDM_Output_Current_7); },
    +[](const PDM_Output_Current_0x501_t& m) { return static_cast<float>(m.PDM_Output_Current_8); },
    +[](const PDM_Output_Current_0x501_t& m) { return static_cast<float>(m.PDM_Output_Current_9); },
    +[](const PDM_Output_Current_0x501_t& m) { return static_cast<float>(m.PDM_Output_Current_10); },
    +[](const PDM_Output_Current_0x501_t& m) { return static_cast<float>(m.PDM_Output_Current_11); },
    +[](const PDM_Output_Current_0x501_t& m) { return static_cast<float>(m.PDM_Output_Current_12); },
    +[](const PDM_Output_Current_0x501_t& m) { return static_cast<float>(m.PDM_Output_Current_13); },
    +[](const PDM_Output_Current_0x501_t& m) { return static_cast<float>(m.PDM_Output_Current_14); },
    +[](const PDM_Output_Current_0x501_t& m) { return static_cast<float>(m.PDM_Output_Current_15); },
    +[](const PDM_Output_Current_0x501_t& m) { return static_cast<float>(m.PDM_Output_Current_16); },
    +[](const PDM_Output_Current_0x501_t& m) { return static_cast<float>(m.PDM_Output_Current_17); },
    +[](const PDM_Output_Current_0x501_t& m) { return static_cast<float>(m.PDM_Output_Current_18); },
    +[](const PDM_Output_Current_0x501_t& m) { return static_cast<float>(m.PDM_Output_Current_19); },
    +[](const PDM_Output_Current_0x501_t& m) { return static_cast<float>(m.PDM_Output_Current_20); },
    +[](const PDM_Output_Current_0x501_t& m) { return static_cast<float>(m.PDM_Output_Current_21); },
    +[](const PDM_Output_Current_0x501_t& m) { return static_cast<float>(m.PDM_Output_Current_22); },
    +[](const PDM_Output_Current_0x501_t& m) { return static_cast<float>(m.PDM_Output_Current_23); },
    +[](const PDM_Output_Current_0x501_t& m) { return static_cast<float>(m.PDM_Output_Current_24); },
    +[](const PDM_Output_Current_0x501_t& m) { return static_cast<float>(m.PDM_Output_Current_25); },
    +[](const PDM_Output_Current_0x501_t& m) { return static_cast<float>(m.PDM_Output_Current_26); },
    +[](const PDM_Output_Current_0x501_t& m) { return static_cast<float>(m.PDM_Output_Current_27); },
    +[](const PDM_Output_Current_0x501_t& m) { return static_cast<float>(m.PDM_Output_Current_28); },
    +[](const PDM_Output_Current_0x501_t& m) { return static_cast<float>(m.PDM_Output_Current_29); },
    +[](const PDM_Output_Current_0x501_t& m) { return static_cast<float>(m.PDM_Output_Current_30); },
    +[](const PDM_Output_Current_0x501_t& m) { return static_cast<float>(m.PDM_Output_Current_31); },
    +[](const PDM_Output_Current_0x501_t& m) { return static_cast<float>(m.PDM_Output_Current_32); },
});
static_assert(kOutputCurrentFields.size() == kOutputCount);

const auto kOutputLoadFields = std::to_array<float (*)(const PDM_Output_Load_0x502_t&)>({
    +[](const PDM_Output_Load_0x502_t& m) { return static_cast<float>(m.PDM_Output_Load_1); },
    +[](const PDM_Output_Load_0x502_t& m) { return static_cast<float>(m.PDM_Output_Load_2); },
    +[](const PDM_Output_Load_0x502_t& m) { return static_cast<float>(m.PDM_Output_Load_3); },
    +[](const PDM_Output_Load_0x502_t& m) { return static_cast<float>(m.PDM_Output_Load_4); },
    +[](const PDM_Output_Load_0x502_t& m) { return static_cast<float>(m.PDM_Output_Load_5); },
    +[](const PDM_Output_Load_0x502_t& m) { return static_cast<float>(m.PDM_Output_Load_6); },
    +[](const PDM_Output_Load_0x502_t& m) { return static_cast<float>(m.PDM_Output_Load_7); },
    +[](const PDM_Output_Load_0x502_t& m) { return static_cast<float>(m.PDM_Output_Load_8); },
    +[](const PDM_Output_Load_0x502_t& m) { return static_cast<float>(m.PDM_Output_Load_9); },
    +[](const PDM_Output_Load_0x502_t& m) { return static_cast<float>(m.PDM_Output_Load_10); },
    +[](const PDM_Output_Load_0x502_t& m) { return static_cast<float>(m.PDM_Output_Load_11); },
    +[](const PDM_Output_Load_0x502_t& m) { return static_cast<float>(m.PDM_Output_Load_12); },
    +[](const PDM_Output_Load_0x502_t& m) { return static_cast<float>(m.PDM_Output_Load_13); },
    +[](const PDM_Output_Load_0x502_t& m) { return static_cast<float>(m.PDM_Output_Load_14); },
    +[](const PDM_Output_Load_0x502_t& m) { return static_cast<float>(m.PDM_Output_Load_15); },
    +[](const PDM_Output_Load_0x502_t& m) { return static_cast<float>(m.PDM_Output_Load_16); },
    +[](const PDM_Output_Load_0x502_t& m) { return static_cast<float>(m.PDM_Output_Load_17); },
    +[](const PDM_Output_Load_0x502_t& m) { return static_cast<float>(m.PDM_Output_Load_18); },
    +[](const PDM_Output_Load_0x502_t& m) { return static_cast<float>(m.PDM_Output_Load_19); },
    +[](const PDM_Output_Load_0x502_t& m) { return static_cast<float>(m.PDM_Output_Load_20); },
    +[](const PDM_Output_Load_0x502_t& m) { return static_cast<float>(m.PDM_Output_Load_21); },
    +[](const PDM_Output_Load_0x502_t& m) { return static_cast<float>(m.PDM_Output_Load_22); },
    +[](const PDM_Output_Load_0x502_t& m) { return static_cast<float>(m.PDM_Output_Load_23); },
    +[](const PDM_Output_Load_0x502_t& m) { return static_cast<float>(m.PDM_Output_Load_24); },
    +[](const PDM_Output_Load_0x502_t& m) { return static_cast<float>(m.PDM_Output_Load_25); },
    +[](const PDM_Output_Load_0x502_t& m) { return static_cast<float>(m.PDM_Output_Load_26); },
    +[](const PDM_Output_Load_0x502_t& m) { return static_cast<float>(m.PDM_Output_Load_27); },
    +[](const PDM_Output_Load_0x502_t& m) { return static_cast<float>(m.PDM_Output_Load_28); },
    +[](const PDM_Output_Load_0x502_t& m) { return static_cast<float>(m.PDM_Output_Load_29); },
    +[](const PDM_Output_Load_0x502_t& m) { return static_cast<float>(m.PDM_Output_Load_30); },
    +[](const PDM_Output_Load_0x502_t& m) { return static_cast<float>(m.PDM_Output_Load_31); },
    +[](const PDM_Output_Load_0x502_t& m) { return static_cast<float>(m.PDM_Output_Load_32); },
});
static_assert(kOutputLoadFields.size() == kOutputCount);

const auto kOutputVoltageFields = std::to_array<float (*)(const PDM_Output_Voltage_0x503_t&)>({
    +[](const PDM_Output_Voltage_0x503_t& m) { return static_cast<float>(m.PDM_Output_Voltage_1); },
    +[](const PDM_Output_Voltage_0x503_t& m) { return static_cast<float>(m.PDM_Output_Voltage_2); },
    +[](const PDM_Output_Voltage_0x503_t& m) { return static_cast<float>(m.PDM_Output_Voltage_3); },
    +[](const PDM_Output_Voltage_0x503_t& m) { return static_cast<float>(m.PDM_Output_Voltage_4); },
    +[](const PDM_Output_Voltage_0x503_t& m) { return static_cast<float>(m.PDM_Output_Voltage_5); },
    +[](const PDM_Output_Voltage_0x503_t& m) { return static_cast<float>(m.PDM_Output_Voltage_6); },
    +[](const PDM_Output_Voltage_0x503_t& m) { return static_cast<float>(m.PDM_Output_Voltage_7); },
    +[](const PDM_Output_Voltage_0x503_t& m) { return static_cast<float>(m.PDM_Output_Voltage_8); },
    +[](const PDM_Output_Voltage_0x503_t& m) { return static_cast<float>(m.PDM_Output_Voltage_9); },
    +[](const PDM_Output_Voltage_0x503_t& m) { return static_cast<float>(m.PDM_Output_Voltage_10); },
    +[](const PDM_Output_Voltage_0x503_t& m) { return static_cast<float>(m.PDM_Output_Voltage_11); },
    +[](const PDM_Output_Voltage_0x503_t& m) { return static_cast<float>(m.PDM_Output_Voltage_12); },
    +[](const PDM_Output_Voltage_0x503_t& m) { return static_cast<float>(m.PDM_Output_Voltage_13); },
    +[](const PDM_Output_Voltage_0x503_t& m) { return static_cast<float>(m.PDM_Output_Voltage_14); },
    +[](const PDM_Output_Voltage_0x503_t& m) { return static_cast<float>(m.PDM_Output_Voltage_15); },
    +[](const PDM_Output_Voltage_0x503_t& m) { return static_cast<float>(m.PDM_Output_Voltage_16); },
    +[](const PDM_Output_Voltage_0x503_t& m) { return static_cast<float>(m.PDM_Output_Voltage_17); },
    +[](const PDM_Output_Voltage_0x503_t& m) { return static_cast<float>(m.PDM_Output_Voltage_18); },
    +[](const PDM_Output_Voltage_0x503_t& m) { return static_cast<float>(m.PDM_Output_Voltage_19); },
    +[](const PDM_Output_Voltage_0x503_t& m) { return static_cast<float>(m.PDM_Output_Voltage_20); },
    +[](const PDM_Output_Voltage_0x503_t& m) { return static_cast<float>(m.PDM_Output_Voltage_21); },
    +[](const PDM_Output_Voltage_0x503_t& m) { return static_cast<float>(m.PDM_Output_Voltage_22); },
    +[](const PDM_Output_Voltage_0x503_t& m) { return static_cast<float>(m.PDM_Output_Voltage_23); },
    +[](const PDM_Output_Voltage_0x503_t& m) { return static_cast<float>(m.PDM_Output_Voltage_24); },
    +[](const PDM_Output_Voltage_0x503_t& m) { return static_cast<float>(m.PDM_Output_Voltage_25); },
    +[](const PDM_Output_Voltage_0x503_t& m) { return static_cast<float>(m.PDM_Output_Voltage_26); },
    +[](const PDM_Output_Voltage_0x503_t& m) { return static_cast<float>(m.PDM_Output_Voltage_27); },
    +[](const PDM_Output_Voltage_0x503_t& m) { return static_cast<float>(m.PDM_Output_Voltage_28); },
    +[](const PDM_Output_Voltage_0x503_t& m) { return static_cast<float>(m.PDM_Output_Voltage_29); },
    +[](const PDM_Output_Voltage_0x503_t& m) { return static_cast<float>(m.PDM_Output_Voltage_30); },
    +[](const PDM_Output_Voltage_0x503_t& m) { return static_cast<float>(m.PDM_Output_Voltage_31); },
    +[](const PDM_Output_Voltage_0x503_t& m) { return static_cast<float>(m.PDM_Output_Voltage_32); },
});
static_assert(kOutputVoltageFields.size() == kOutputCount);

// A publisher's builder lives as long as the publisher, so the list from the
// previous message is reused when it is already the right length.
template <typename List>
bool reusable(bool has, const List& list, std::size_t count)
{
    return has && list.size() == count;
}

// The DBC's status values onto the schema's enum. Each output's status is its
// own generated enum type with the same enumerants, hence the template.
template <typename EnumT>
PdmOutputStatusEnum toCapnp(EnumT input)
{
    switch (input)
    {
        case EnumT::Output_Off:
            return PdmOutputStatusEnum::OFF;

        case EnumT::Output_On:
            return PdmOutputStatusEnum::ON;

        case EnumT::Output_Fault_Error:
            return PdmOutputStatusEnum::FAULT_ERROR;

        case EnumT::Output_Over_Current_Error:
            return PdmOutputStatusEnum::OVER_CURRENT_ERROR;

        case EnumT::Output_Retries_Reached:
            return PdmOutputStatusEnum::RETRIES_REACHED;
    }

    // A raw value the DBC does not name. The schema has no "unknown", and off is
    // what the node has always published for one.
    return PdmOutputStatusEnum::OFF;
}

template <typename Msg, typename Table, typename Builder>
void fillFloats(const Msg& m, const Table& fields, Builder out)
{
    auto values = reusable(out.hasValues(), out.getValues(), fields.size())
                      ? out.getValues()
                      : out.initValues(static_cast<unsigned>(fields.size()));
    for (std::size_t i = 0; i < fields.size(); ++i)
    {
        values.set(static_cast<unsigned>(i), fields[i](m));
    }
}

}  // namespace

void fillInputState(const PDM_Input_State_0x500_t& m, MotecPdmInputState::Builder out)
{
    out.setResetSource(static_cast<std::uint8_t>(m.PDM_Reset_Source));
    out.setRail9v5Volts(static_cast<float>(m.PDM_9V5_Internal_Rail_Voltage));
    out.setTotalCurrentA(static_cast<float>(m.PDM_Total_Current));
    out.setGlobalErrorFlag(static_cast<std::uint8_t>(m.PDM_Global_Error_Flag));
    out.setBatteryVolts(static_cast<float>(m.PDM_Battery_Voltage));
    out.setInternalTempC(static_cast<float>(m.PDM_Internal_Temperature));

    auto inputs = reusable(out.hasInputs(), out.getInputs(), kInputFields.size())
                      ? out.getInputs()
                      : out.initInputs(static_cast<unsigned>(kInputFields.size()));
    for (std::size_t i = 0; i < kInputFields.size(); ++i)
    {
        inputs.set(static_cast<unsigned>(i), kInputFields[i](m));
    }
}

void fillInputVoltage(const PDM_Input_Voltage_0x505_t& m, MotecPdmInputVoltage::Builder out)
{
    fillFloats(m, kInputVoltageFields, out);
}

void fillInfo(const PDM_Input_Voltage_0x505_t& m, MotecPdmInfo::Builder out)
{
    out.setSerialNumberLow(static_cast<std::uint8_t>(m.PDM_Serial_Number_Low));
    out.setSerialNumberHigh(static_cast<std::uint8_t>(m.PDM_Serial_Number_High));
    out.setFwVersionLetter(static_cast<std::uint8_t>(m.PDM_Firmware_Version_Letter));
    out.setFwVersionMinor(static_cast<std::uint8_t>(m.PDM_Firmware_Version_Minor));
    out.setFwVersionMajor(static_cast<std::uint8_t>(m.PDM_Firmware_Version_Major));
}

void fillOutputCurrent(const PDM_Output_Current_0x501_t& m, MotecPdmOutputCurrent::Builder out)
{
    fillFloats(m, kOutputCurrentFields, out);
}

void fillOutputLoad(const PDM_Output_Load_0x502_t& m, MotecPdmOutputLoad::Builder out)
{
    fillFloats(m, kOutputLoadFields, out);
}

void fillOutputVoltage(const PDM_Output_Voltage_0x503_t& m, MotecPdmOutputVoltage::Builder out)
{
    fillFloats(m, kOutputVoltageFields, out);
}

void fillOutputStatus(const PDM_Output_Status_0x504_t& m, MotecPdmOutputStatus::Builder out)
{
    auto values = reusable(out.hasValues(), out.getValues(), kOutputCount)
                      ? out.getValues()
                      : out.initValues(static_cast<unsigned>(kOutputCount));
    values.set(0, toCapnp(m.PDM_Output_Status_1));
    values.set(1, toCapnp(m.PDM_Output_Status_2));
    values.set(2, toCapnp(m.PDM_Output_Status_3));
    values.set(3, toCapnp(m.PDM_Output_Status_4));
    values.set(4, toCapnp(m.PDM_Output_Status_5));
    values.set(5, toCapnp(m.PDM_Output_Status_6));
    values.set(6, toCapnp(m.PDM_Output_Status_7));
    values.set(7, toCapnp(m.PDM_Output_Status_8));
    values.set(8, toCapnp(m.PDM_Output_Status_9));
    values.set(9, toCapnp(m.PDM_Output_Status_10));
    values.set(10, toCapnp(m.PDM_Output_Status_11));
    values.set(11, toCapnp(m.PDM_Output_Status_12));
    values.set(12, toCapnp(m.PDM_Output_Status_13));
    values.set(13, toCapnp(m.PDM_Output_Status_14));
    values.set(14, toCapnp(m.PDM_Output_Status_15));
    values.set(15, toCapnp(m.PDM_Output_Status_16));
    values.set(16, toCapnp(m.PDM_Output_Status_17));
    values.set(17, toCapnp(m.PDM_Output_Status_18));
    values.set(18, toCapnp(m.PDM_Output_Status_19));
    values.set(19, toCapnp(m.PDM_Output_Status_20));
    values.set(20, toCapnp(m.PDM_Output_Status_21));
    values.set(21, toCapnp(m.PDM_Output_Status_22));
    values.set(22, toCapnp(m.PDM_Output_Status_23));
    values.set(23, toCapnp(m.PDM_Output_Status_24));
    values.set(24, toCapnp(m.PDM_Output_Status_25));
    values.set(25, toCapnp(m.PDM_Output_Status_26));
    values.set(26, toCapnp(m.PDM_Output_Status_27));
    values.set(27, toCapnp(m.PDM_Output_Status_28));
    values.set(28, toCapnp(m.PDM_Output_Status_29));
    values.set(29, toCapnp(m.PDM_Output_Status_30));
    values.set(30, toCapnp(m.PDM_Output_Status_31));
    values.set(31, toCapnp(m.PDM_Output_Status_32));
}

}  // namespace motec_pdm
