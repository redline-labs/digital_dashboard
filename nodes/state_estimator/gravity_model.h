// SPDX-License-Identifier: GPL-3.0-or-later
//
// The gravity model the node runs on: NGS's DEFLEC2022, loaded at start from
// the verbatim files under models/deflec2022, or normal gravity when it is
// switched off or cannot be loaded. A model that is missing (or is an LFS
// pointer because the image was built from a checkout without `git lfs pull`)
// does not stop the node -- normal gravity is what it ran on before -- but the
// health check says so, with the reason.

#ifndef STATE_ESTIMATOR_GRAVITY_MODEL_H
#define STATE_ESTIMATOR_GRAVITY_MODEL_H

#include "node_config.h"

#include "geodesy/gravity.h"

#include <memory>
#include <string>

namespace state_estimator
{

struct LoadedGravity
{
    std::shared_ptr<const geodesy::GravityModel> model;  // null: normal gravity
    bool healthy = true;  // false: asked for and not loaded
    std::string summary;  // for the log and the health check
};

LoadedGravity loadGravity(const NodeConfig& config);

}  // namespace state_estimator

#endif  // STATE_ESTIMATOR_GRAVITY_MODEL_H
