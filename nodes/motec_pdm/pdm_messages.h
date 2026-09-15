// SPDX-License-Identifier: GPL-3.0-or-later
//
// Decoded PDM messages onto their capnp schemas.
//
// Out of main.cpp so the list lengths can be tested against the schema. Every
// list here is declared $fixedLength in schemas/motec_pdm.capnp, and a consumer
// compiles its expressions against that declared length: a message carrying any
// other count is dropped whole by pub_sub::ExpressionEvaluator. The node once
// published 24 input flags against a declared 23, and nothing that read
// `inputs` ever produced a value. motec_pdm_test_messages pins every list.

#ifndef MOTEC_PDM_PDM_MESSAGES_H_
#define MOTEC_PDM_PDM_MESSAGES_H_

#include "dbc_motec_pdm_generic_output_parser.h"
#include "motec_pdm.capnp.h"

#include <cstddef>

namespace motec_pdm
{

// The PDM's channel counts, as the DBC defines them. The tables in
// pdm_messages.cpp are sized from their initializers and checked against these.
inline constexpr std::size_t kInputCount = 23;
inline constexpr std::size_t kOutputCount = 32;

using namespace dbc_motec_pdm_generic_output;

void fillInputState(const PDM_Input_State_0x500_t& m, MotecPdmInputState::Builder out);

// 0x505 carries both: the input voltages on its first pages, and the serial and
// firmware on its last.
void fillInputVoltage(const PDM_Input_Voltage_0x505_t& m, MotecPdmInputVoltage::Builder out);
void fillInfo(const PDM_Input_Voltage_0x505_t& m, MotecPdmInfo::Builder out);

void fillOutputCurrent(const PDM_Output_Current_0x501_t& m, MotecPdmOutputCurrent::Builder out);
void fillOutputLoad(const PDM_Output_Load_0x502_t& m, MotecPdmOutputLoad::Builder out);
void fillOutputVoltage(const PDM_Output_Voltage_0x503_t& m, MotecPdmOutputVoltage::Builder out);
void fillOutputStatus(const PDM_Output_Status_0x504_t& m, MotecPdmOutputStatus::Builder out);

}  // namespace motec_pdm

#endif  // MOTEC_PDM_PDM_MESSAGES_H_
