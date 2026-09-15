// SPDX-License-Identifier: GPL-3.0-or-later
#include "xbus_fields.h"

namespace mti610_node
{

void fill(::XbusTemperature::Builder builder, const xbus::Temperature& value)
{
    builder.setTemperatureC(value.temperatureC);
}

void fill(::XbusUtcTime::Builder builder, const xbus::UtcTime& value)
{
    builder.setYear(value.year);
    builder.setMonth(value.month);
    builder.setDay(value.day);
    builder.setHour(value.hour);
    builder.setMinute(value.minute);
    builder.setSecond(value.second);
    builder.setNanosecond(value.nanosecond);
    builder.setFlags(value.flags);
    builder.setDateValid(value.dateValid());
    builder.setTimeValid(value.timeValid());
    builder.setFullyResolved(value.fullyResolved());
}

void fill(::XbusBaroPressure::Builder builder, const xbus::BaroPressure& value)
{
    builder.setPressurePa(value.pressurePa);
}

void fill(::XbusDeltaV::Builder builder, const xbus::DeltaV& value)
{
    builder.setDeltaVXMps(value.xMps);
    builder.setDeltaVYMps(value.yMps);
    builder.setDeltaVZMps(value.zMps);
}

void fill(::XbusAcceleration::Builder builder, const xbus::Acceleration& value)
{
    builder.setAccelerationXMps2(value.xMps2);
    builder.setAccelerationYMps2(value.yMps2);
    builder.setAccelerationZMps2(value.zMps2);
}

void fill(::XbusAccelerationHr::Builder builder, const xbus::AccelerationHr& value)
{
    builder.setAccelerationXMps2(value.xMps2);
    builder.setAccelerationYMps2(value.yMps2);
    builder.setAccelerationZMps2(value.zMps2);
}

void fill(::XbusRateOfTurn::Builder builder, const xbus::RateOfTurn& value)
{
    builder.setRateOfTurnXRadps(value.xRadps);
    builder.setRateOfTurnYRadps(value.yRadps);
    builder.setRateOfTurnZRadps(value.zRadps);
}

void fill(::XbusRateOfTurnHr::Builder builder, const xbus::RateOfTurnHr& value)
{
    builder.setRateOfTurnXRadps(value.xRadps);
    builder.setRateOfTurnYRadps(value.yRadps);
    builder.setRateOfTurnZRadps(value.zRadps);
}

void fill(::XbusDeltaQ::Builder builder, const xbus::DeltaQ& value)
{
    builder.setDeltaQW(value.w);
    builder.setDeltaQX(value.x);
    builder.setDeltaQY(value.y);
    builder.setDeltaQZ(value.z);
}

void fill(::XbusMagneticField::Builder builder, const xbus::MagneticField& value)
{
    builder.setMagneticFieldXAu(value.xAu);
    builder.setMagneticFieldYAu(value.yAu);
    builder.setMagneticFieldZAu(value.zAu);
}

}  // namespace mti610_node
