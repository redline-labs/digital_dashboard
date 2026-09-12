// SPDX-License-Identifier: GPL-3.0-or-later
//
// The one list of MTData2 data identifiers this tree understands.
//
// Everything else derives from it: the DataId enum, the identifier-to-name
// mapping, the parse dispatch, and (in the node) the topic each identifier is
// published on. ADDING AN IDENTIFIER IS ONE ROW HERE plus a struct with a
// constexpr parse() in mtdata2.h -- if you add the row and forget the struct,
// the dispatch switch fails to compile, which is the point.
//
// Columns:
//   id        the 16-bit identifier with its FORMAT NIBBLE ZEROED, which is
//             what xsdataidentifier.h's enumerators carry
//   Name      the struct in xbus::, and the DataId enumerator
//   snake     the display name, and the last segment of the node's zenoh topic
//   elements  how many real-valued components the payload holds, or 0 when the
//             payload has a fixed binary layout that ignores the format nibble
//
// THIS IS THE SET AN MTi-610 EMITS, AND DELIBERATELY NOTHING MORE.
//
// The 610 is the IMU member of the 600-series: it has no orientation filter.
// Two independent places in the LLCP say so -- Table 17's product columns, and
// the default-configuration table in section 4.2, which groups
// "MTi-1/10/100/610 IMU" with Delta_q, Delta_v and Mag Field and gives it no
// quaternion. So the orientation group (0x20xy), FreeAcceleration (0x4030, which
// needs the filter to remove gravity), the position, GNSS and velocity groups,
// the raw sensor group (0xA0x0, 10/100-series only) and the DeviceId/LocationId
// data identifiers (0xE080/0xE090, likewise) are all absent on purpose.
//
// Adding them is not a matter of adding rows: an MTi-620, -630 or -670 has
// outputs this library has never seen a byte of, and a row here is a claim
// that the struct beneath it was checked against something. See docs/mti610.md.

#ifndef XBUS_DATA_TABLE_H
#define XBUS_DATA_TABLE_H

#define XBUS_DATA_TABLE(X)                                             \
    X(0x0810, Temperature,      "temperature",        1)               \
    X(0x1010, UtcTime,          "utc_time",           0)               \
    X(0x1020, PacketCounter,    "packet_counter",     0)               \
    X(0x1060, SampleTimeFine,   "sample_time_fine",   0)               \
    X(0x1070, SampleTimeCoarse, "sample_time_coarse", 0)               \
    X(0x3010, BaroPressure,     "baro_pressure",      0)               \
    X(0x4010, DeltaV,           "delta_v",            3)               \
    X(0x4020, Acceleration,     "acceleration",       3)               \
    X(0x4040, AccelerationHr,   "acceleration_hr",    3)               \
    X(0x8020, RateOfTurn,       "rate_of_turn",       3)               \
    X(0x8030, DeltaQ,           "delta_q",            4)               \
    X(0x8040, RateOfTurnHr,     "rate_of_turn_hr",    3)               \
    X(0xC020, MagneticField,    "magnetic_field",     3)               \
    X(0xE010, StatusByte,       "status_byte",        0)               \
    X(0xE020, StatusWord,       "status_word",        0)

#endif // XBUS_DATA_TABLE_H
