// SPDX-License-Identifier: GPL-3.0-or-later
//
// The decoded LTC message onto its schema.
//
// Out of main.cpp so it can be tested without a bus. Two things here are worth
// pinning: the seven fault flags, which are one-bit value tables and are next
// to each other, and the sensor state, which is a value table mapped to a
// second enumeration by hand -- a mapping that compiles whatever it says.
#ifndef MOTEC_LTC_MESSAGES_H_
#define MOTEC_LTC_MESSAGES_H_

#include "dbc_motec_ltc_rev1_parser.h"
#include "motec_ltc.capnp.h"

namespace motec_ltc
{

void fillTelemetry(const dbc_motec_ltc_rev1::LTC_1_ID1_t& m, MotecLtcTelemetry::Builder out);

}  // namespace motec_ltc

#endif  // MOTEC_LTC_MESSAGES_H_
