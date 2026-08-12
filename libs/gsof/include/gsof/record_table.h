// SPDX-License-Identifier: GPL-3.0-or-later
//
// The one list of GSOF records this tree understands.
//
// Everything else derives from it: the RecordType enum, the Record variant,
// the type-to-name mapping, the parse dispatch, and (in the node) the topic
// each record is published on. ADDING A RECORD TYPE IS ONE ROW HERE plus a
// struct with a constexpr parse() in records.h -- if you add the row and
// forget the struct, the dispatch switch fails to compile, which is the point.
//
// Columns:
//   id     the wire byte, from the GSOF message table in the Trimble ICD
//   Name   the struct in gsof::, and the RecordType enumerator
//   snake  the display name, and the last segment of the node's zenoh topic
//
// The list is the subset a BD992 can emit plus the two INS records, which a
// BD992 never sends but which cost one row each and are covered by captures we
// already have. It is deliberately not the whole ICD: an unmodelled record is
// not an error anywhere in this library -- the record framing is
// self-describing, so it is skipped, counted, and passed through as raw bytes.

#ifndef GSOF_RECORD_TABLE_H
#define GSOF_RECORD_TABLE_H

#define GSOF_RECORD_TABLE(X)                                    \
    X(  1, PositionTime,      "position_time")                  \
    X(  2, LatLongHeight,     "lat_long_height")                \
    X(  3, EcefPosition,      "ecef_position")                  \
    X(  6, EcefDelta,         "ecef_delta")                     \
    X(  7, TangentPlaneDelta, "tangent_plane_delta")            \
    X(  8, Velocity,          "velocity")                       \
    X(  9, DopInfo,           "dop_info")                       \
    X( 10, ClockInfo,         "clock_info")                     \
    X( 11, PositionVcv,       "position_vcv")                   \
    X( 12, PositionSigma,     "position_sigma")                 \
    X( 15, ReceiverSerial,    "receiver_serial")                \
    X( 16, CurrentTimeUtc,    "current_time_utc")               \
    X( 27, AttitudeInfo,      "attitude_info")                  \
    X( 33, AllSvBrief,        "all_sv_brief")                   \
    X( 34, AllSvDetailed,     "all_sv_detailed")                \
    X( 35, ReceivedBase,      "received_base")                  \
    X( 37, BatteryMemory,     "battery_memory")                 \
    X( 38, PositionType,      "position_type")                  \
    X( 40, LbandStatus,       "lband_status")                   \
    X( 41, BasePosition,      "base_position")                  \
    X( 49, InsFullNav,        "ins_full_nav")                   \
    X( 50, InsRms,            "ins_rms")

#endif // GSOF_RECORD_TABLE_H
