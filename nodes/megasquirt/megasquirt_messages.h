// SPDX-License-Identifier: GPL-3.0-or-later
//
// The decoded Megasquirt dash frames onto their schema.
//
// Out of main.cpp so it can be tested: the five dash frames carry 20 readings
// between them, and every way of getting one wrong -- a field from the wrong
// frame, a temperature in the wrong unit, two fields transposed -- produces a
// number that looks like a reading. Nothing downstream can tell.
//
// No zenoh here, only the generated decoder and the generated schema, so
// megasquirt_test_messages runs without a bus.
#ifndef MEGASQUIRT_MESSAGES_H_
#define MEGASQUIRT_MESSAGES_H_

#include "dbc_megasquirt_dash_data_parser.h"
#include "megasquirt.capnp.h"

namespace megasquirt
{

// One MegasquirtDash from a complete set of dash0..dash4.
void fillDash(const dbc_megasquirt_dash_data::dbc_megasquirt_dash_data_parser::db_t& db, MegasquirtDash::Builder out);

}  // namespace megasquirt

#endif  // MEGASQUIRT_MESSAGES_H_
