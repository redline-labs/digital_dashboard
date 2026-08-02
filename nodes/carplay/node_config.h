// SPDX-License-Identifier: GPL-3.0-or-later
//
// The CarPlay node's YAML configuration -- currently the manufacturer button,
// whose artwork is the one thing that cannot reasonably be a command-line flag.
//
// Every field has a working default, so the node runs with no config file at
// all; see configs/carplay/carplay.yaml for the documented form.
#ifndef CARPLAY_NODE_CONFIG_H_
#define CARPLAY_NODE_CONFIG_H_

#include "airplay/receiver.h"

#include <string>

namespace carplay
{

struct NodeConfig
{
    // Draw CarPlay's own UI in its night theme. There is no light sensor here
    // yet, so this is whatever the vehicle is configured with; see
    // docs/carplay_bringup.md stage 11 for hooking it to the headlights.
    bool night_mode = false;

    // The node's defaults deliberately differ from the library's: a head unit
    // wants the button, whereas airplay::Receiver defaults to advertising
    // nothing so that a caller has to ask for it. No icon is defaulted --
    // artwork only ever comes from a config file.
    airplay::OemButtonConfig oem_button{.enabled = true, .label = "Dashboard"};
};

// Reads `path`, layering it over the defaults in NodeConfig. Icon paths inside
// the file resolve against the file's own directory, so a config can be handed
// around with its artwork.
//
// Returns false and leaves `out` untouched when the file cannot be read or
// parsed; a missing *key* is not an error, it just keeps the default. Errors
// are logged with the [node] prefix.
bool loadNodeConfig(const std::string& path, NodeConfig& out);

// Reads one PNG into an OemIcon, taking the dimensions from its IHDR chunk.
// Returns false (and logs) if the file is missing or is not a PNG.
bool loadOemIcon(const std::string& path, bool prerendered, airplay::OemIcon& out);

}  // namespace carplay

#endif  // CARPLAY_NODE_CONFIG_H_
