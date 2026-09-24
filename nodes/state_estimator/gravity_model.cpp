// SPDX-License-Identifier: GPL-3.0-or-later
#include "gravity_model.h"

#include "core/core.h"
#include "deflec/model.h"

namespace state_estimator
{

LoadedGravity loadGravity(const NodeConfig& config)
{
    if (!config.gravity.deflection) return {nullptr, true, "normal gravity (gravity.deflection is off)"};
    const std::string dir = config.gravity.modelDir.empty() ? core::paths::resource("models/deflec2022")
                                                            : core::paths::expand(config.gravity.modelDir);
    auto model = deflec::Model::open(dir);
    if (!model) return {nullptr, false, "normal gravity: DEFLEC2022 not loaded: " + model.error().message};
    const auto rows = model->eta().rows(), cols = model->eta().cols();
    auto gravity = std::make_shared<const deflec::DeflectedGravity>(std::make_shared<const deflec::Model>(std::move(*model)));
    return {std::move(gravity), true,
            "DEFLEC2022 from " + dir + " (" + std::to_string(rows) + " x " + std::to_string(cols) + ")"};
}

}  // namespace state_estimator
