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
// The list covers every record type a BD992 was seen to emit with all message
// types enabled -- 28 of the rows below were read off a live receiver's socket
// -- plus records 6, 7, 27 and the two INS records, which that receiver had no
// reason to send (no base, no second antenna, no IMU) but which cost one row
// each and are covered by captures we already have.
//
// It is deliberately not the whole ICD: an unmodelled record is not an error
// anywhere in this library -- the record framing is self-describing, so it is
// skipped, counted, and passed through as raw bytes. But an unmodelled record
// IS a topic nobody can subscribe to, so "the receiver sends it" is the bar for
// adding a row.

#ifndef GSOF_RECORD_TABLE_H
#define GSOF_RECORD_TABLE_H

#define GSOF_RECORD_TABLE(X)                            \
    X(  1, PositionTime,        "position_time")        \
    X(  2, LatLongHeight,       "lat_long_height")      \
    X(  3, EcefPosition,        "ecef_position")        \
    X(  6, EcefDelta,           "ecef_delta")           \
    X(  7, TangentPlaneDelta,   "tangent_plane_delta")  \
    X(  8, Velocity,            "velocity")             \
    X(  9, DopInfo,             "dop_info")             \
    X( 10, ClockInfo,           "clock_info")           \
    X( 11, PositionVcv,         "position_vcv")         \
    X( 12, PositionSigma,       "position_sigma")       \
    X( 13, SvBriefInfo,         "sv_brief")             \
    X( 14, SvDetailInfo,        "sv_detailed")          \
    X( 15, ReceiverSerial,      "receiver_serial")      \
    X( 16, CurrentTimeUtc,      "current_time_utc")     \
    X( 27, AttitudeInfo,        "attitude_info")        \
    X( 28, ReceiverDiagnostics, "receiver_diagnostics") \
    X( 33, AllSvBrief,          "all_sv_brief")         \
    X( 34, AllSvDetailed,       "all_sv_detailed")      \
    X( 35, ReceivedBase,        "received_base")        \
    X( 37, BatteryMemory,       "battery_memory")       \
    X( 38, PositionType,        "position_type")        \
    X( 40, LbandStatus,         "lband_status")         \
    X( 41, BasePosition,        "base_position")        \
    X( 48, AllSvDetailedPage,   "all_sv_detailed_page") \
    X( 49, InsFullNav,          "ins_full_nav")         \
    X( 50, InsRms,              "ins_rms")              \
    X( 62, CodePosition,        "code_position")        \
    X( 70, LatLongMslHeight,    "lat_long_msl_height")  \
    X( 74, SecondAntennaSigma,  "second_antenna_sigma") \
    X( 91, NavMessageAuth,      "nav_message_auth")     \
    X( 92, IonoGuardInfo,       "ionoguard_info")       \
    X( 96, IonoGuardSummary,    "ionoguard_summary")

#endif // GSOF_RECORD_TABLE_H
