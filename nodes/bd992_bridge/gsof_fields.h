// SPDX-License-Identifier: GPL-3.0-or-later
//
// One GSOF record onto its schema, once per record type.
//
// Out of publishers.cpp so it can be tested without a receiver or a bus. The
// unit conversions live here and nowhere else -- the library keeps radians
// because that is what the wire carries, and every schema is in degrees -- so
// a record published in radians would read as a position a few hundred
// kilometres away, which is a plausible position.
#ifndef BD992_NODE_GSOF_FIELDS_H
#define BD992_NODE_GSOF_FIELDS_H

#include "gsof/records.h"

#include "bd992.capnp.h"
#include "gsof_attitude.capnp.h"
#include "gsof_common.capnp.h"
#include "gsof_ins.capnp.h"
#include "gsof_integrity.capnp.h"
#include "gsof_position.capnp.h"
#include "gsof_satellites.capnp.h"
#include "gsof_status.capnp.h"

#include <cstdint>

namespace bd992_node
{

void fillTime(::GsofGpsTime::Builder time, std::uint16_t week, std::uint32_t timeOfWeekMs);
void fill(::GsofPositionTime::Builder out, const gsof::PositionTime& in);
void fill(::GsofLatLongHeight::Builder out, const gsof::LatLongHeight& in);
void fill(::GsofEcefPosition::Builder out, const gsof::EcefPosition& in);
void fill(::GsofEcefDelta::Builder out, const gsof::EcefDelta& in);
void fill(::GsofTangentPlaneDelta::Builder out, const gsof::TangentPlaneDelta& in);
void fill(::GsofVelocity::Builder out, const gsof::Velocity& in);
void fill(::GsofDopInfo::Builder out, const gsof::DopInfo& in);
void fill(::GsofClockInfo::Builder out, const gsof::ClockInfo& in);
void fill(::GsofPositionVcv::Builder out, const gsof::PositionVcv& in);
void fill(::GsofPositionSigma::Builder out, const gsof::PositionSigma& in);
void fill(::GsofReceiverSerial::Builder out, const gsof::ReceiverSerial& in);
void fill(::GsofCurrentTimeUtc::Builder out, const gsof::CurrentTimeUtc& in);
void fill(::GsofAttitudeInfo::Builder out, const gsof::AttitudeInfo& in);
void fill(::GsofSvBriefInfo::Builder out, const gsof::SvBriefInfo& in);
void fill(::GsofSvDetailInfo::Builder out, const gsof::SvDetailInfo& in);
void fill(::GsofAllSvDetailedPage::Builder out, const gsof::AllSvDetailedPage& in);
void fill(::GsofAllSvBrief::Builder out, const gsof::AllSvBrief& in);
void fill(::GsofAllSvDetailed::Builder out, const gsof::AllSvDetailed& in);
void fill(::GsofReceivedBase::Builder out, const gsof::ReceivedBase& in);
void fill(::GsofBatteryMemory::Builder out, const gsof::BatteryMemory& in);
void fill(::GsofPositionType::Builder out, const gsof::PositionType& in);
void fill(::GsofLbandStatus::Builder out, const gsof::LbandStatus& in);
void fill(::GsofBasePosition::Builder out, const gsof::BasePosition& in);
void fill(::GsofCodePosition::Builder out, const gsof::CodePosition& in);
void fill(::GsofLatLongMslHeight::Builder out, const gsof::LatLongMslHeight& in);
void fill(::GsofReceiverDiagnostics::Builder out, const gsof::ReceiverDiagnostics& in);
void fill(::GsofSecondAntennaSigma::Builder out, const gsof::SecondAntennaSigma& in);
void fill(::GsofNavMessageAuth::Builder out, const gsof::NavMessageAuth& in);
void fill(::GsofIonoGuardInfo::Builder out, const gsof::IonoGuardInfo& in);
void fill(::GsofIonoGuardSummary::Builder out, const gsof::IonoGuardSummary& in);
void fill(::GsofInsFullNav::Builder out, const gsof::InsFullNav& in);
void fill(::GsofInsRms::Builder out, const gsof::InsRms& in);

}  // namespace bd992_node

#endif  // BD992_NODE_GSOF_FIELDS_H
