// SPDX-License-Identifier: GPL-3.0-or-later
//
// The estimator's structs onto their schemas. The one place radians become
// degrees and the library's enum becomes the wire's.

#ifndef STATE_ESTIMATOR_STATE_FIELDS_H
#define STATE_ESTIMATOR_STATE_FIELDS_H

#include "vehicle_state.capnp.h"

#include "vehicle_estimator/estimator.h"

namespace state_estimator
{

void fill(::VehicleState::Builder out, const vehicle_estimator::VehicleState& in);
void fill(::VehicleEstimatorStatus::Builder out, const vehicle_estimator::EstimatorStatus& in);

}  // namespace state_estimator

#endif  // STATE_ESTIMATOR_STATE_FIELDS_H
