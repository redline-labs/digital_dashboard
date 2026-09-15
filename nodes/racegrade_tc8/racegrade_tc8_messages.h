// SPDX-License-Identifier: GPL-3.0-or-later
//
// The decoded E888 messages onto their schemas.
//
// Out of main.cpp so it can be tested without a bus. The E888 carries eight
// voltages, eight thermocouples and four frequencies in one multiplexed
// message, in that order and with nothing in the numbers to tell them apart:
// a mapping that reads TC3 into temperature4 is a plausible dashboard.
#ifndef RACEGRADE_TC8_MESSAGES_H_
#define RACEGRADE_TC8_MESSAGES_H_

#include "dbc_motec_e888_rev1_parser.h"
#include "racegrade_tc8_signals.capnp.h"

namespace racegrade_tc8
{

void fillInputs(const dbc_motec_e888_rev1::Inputs_t& msg, RaceGradeTc8Inputs::Builder outputs);
void fillDiagnostics(const dbc_motec_e888_rev1::Diagnostics_t& msg,
                     RaceGradeTc8Diagnostics::Builder outputs);

}  // namespace racegrade_tc8

#endif  // RACEGRADE_TC8_MESSAGES_H_
