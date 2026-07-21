// SPDX-License-Identifier: GPL-3.0-or-later
//
// Hardware-free simulation mode. Publishes a synthetic CarPlay session
// (encoded H.264 test pattern, PCM tone, and rotating metadata) on the real
// zenoh topics so the entire dashboard side -- video decode/render, touch
// round-trip, audio playback, and the supplemental metadata widgets -- can be
// exercised end-to-end without an iPhone, MFi chip, or Linux host.
#ifndef CARPLAY_SIMULATE_H_
#define CARPLAY_SIMULATE_H_

#include "zenoh_bridge.h"

#include <atomic>

namespace carplay
{

// Blocks until `stop` is set, publishing a synthetic session.
// Returns false if the H.264 encoder could not be initialised.
bool runSimulation(ZenohBridge& bridge, std::atomic<bool>& stop, int width, int height, int fps);

}  // namespace carplay

#endif  // CARPLAY_SIMULATE_H_
