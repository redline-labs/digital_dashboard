// SPDX-License-Identifier: GPL-3.0-or-later
//
// One XBus data item onto its schema.
//
// Out of publishers.cpp so it can be tested without a device or a bus. The axes
// and the units are the whole content of an inertial reading: a transposed pair
// reads as a vehicle doing something else entirely, and nothing downstream can
// tell. The sample header stays in publishers.cpp -- it is assembled from the
// node's own view of the message, not from one item.
#ifndef MTI610_NODE_XBUS_FIELDS_H
#define MTI610_NODE_XBUS_FIELDS_H

#include "xbus/mtdata2.h"

#include "xbus_environment.capnp.h"
#include "xbus_inertial.capnp.h"
#include "xbus_timestamp.capnp.h"

namespace mti610_node
{

void fill(::XbusTemperature::Builder builder, const xbus::Temperature& value);
void fill(::XbusUtcTime::Builder builder, const xbus::UtcTime& value);
void fill(::XbusBaroPressure::Builder builder, const xbus::BaroPressure& value);
void fill(::XbusDeltaV::Builder builder, const xbus::DeltaV& value);
void fill(::XbusAcceleration::Builder builder, const xbus::Acceleration& value);
void fill(::XbusAccelerationHr::Builder builder, const xbus::AccelerationHr& value);
void fill(::XbusRateOfTurn::Builder builder, const xbus::RateOfTurn& value);
void fill(::XbusRateOfTurnHr::Builder builder, const xbus::RateOfTurnHr& value);
void fill(::XbusDeltaQ::Builder builder, const xbus::DeltaQ& value);
void fill(::XbusMagneticField::Builder builder, const xbus::MagneticField& value);

}  // namespace mti610_node

#endif  // MTI610_NODE_XBUS_FIELDS_H
