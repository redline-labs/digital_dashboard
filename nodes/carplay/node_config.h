// SPDX-License-Identifier: GPL-3.0-or-later
//
// Everything the CarPlay node is configured with: what the vehicle presents to
// the phone, and the bring-up knobs that take the pipeline one layer at a time.
//
// One struct rather than one per layer. The presentation half comes from the
// YAML (the manufacturer button's artwork is the one thing that cannot
// reasonably be a command-line flag) and the bring-up half from the command
// line, but both are read once in main() and passed down unchanged -- so adding
// a knob is one field here, not a field in each of three structs that copy each
// other.
//
// Every field has a working default, so the node runs with no config file at
// all; see configs/carplay/carplay.yaml for the documented form.
#ifndef CARPLAY_NODE_CONFIG_H_
#define CARPLAY_NODE_CONFIG_H_

#include "airplay/receiver.h"
#include "iap2/messages.h"
#include "location_fix.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace carplay
{

// Who the vehicle says it is. The phone records some of this against the
// pairing and shows some of it to the user, and it reaches the phone twice by
// two different routes -- iAP2 identification and the AirPlay /info -- which is
// why it lives here once rather than being set at each.
struct VehicleIdentity
{
    std::string name = "Dashboard";
    std::string model = "MercedesDashboard1,1";
    std::string manufacturer = "Dashboard";
    std::string serial_number = "0123456";
    std::string firmware_version = "1.0.0";
    std::string hardware_version = "1.0";

    // Which side the driver sits on. CarPlay mirrors its own layout for this --
    // the reason it is worth getting right rather than leaving at a default.
    bool right_hand_drive = false;

    iap2::EngineType engine_type = iap2::EngineType::kDiesel;

    // The language CarPlay speaks, and the ones it may offer. IETF tags.
    std::string language = "en";
    std::vector<std::string> supported_languages = {"en", "de"};
};

// The panel CarPlay draws on.
struct DisplayConfig
{
    uint32_t width_px = 800;
    uint32_t height_px = 600;
    uint32_t fps = 30;

    // The panel's physical width in millimetres. The height is derived from it
    // and the pixel aspect ratio, so only one is needed. CarPlay uses this to
    // size text and touch targets to something the hand actually meets --
    // getting it wrong gives a UI that is legible on a desk and not in a car.
    uint32_t physical_width_mm = 200;

    // Which input CarPlay lays its own UI out for. Both the touchscreen and the
    // rotary controller are always advertised; this only says which is primary.
    airplay::PrimaryInput primary_input = airplay::PrimaryInput::Touch;
};

struct NodeConfig
{
    VehicleIdentity vehicle;
    DisplayConfig display;

    // Advertised as deviceID and as the Bluetooth address in GET /info. It is
    // a locally administered MAC (the 0x02 bit), which is what an accessory
    // without a real one should present.
    std::string device_id = "02:00:00:00:00:01";

    // Draw CarPlay's own UI in its night theme. There is no light sensor here
    // yet, so this is whatever the vehicle is configured with; see
    // docs/carplay_bringup.md stage 11 for hooking it to the headlights.
    bool night_mode = false;

    // The node's defaults deliberately differ from the library's: a head unit
    // wants the button, whereas airplay::Receiver defaults to advertising
    // nothing so that a caller has to ask for it. No icon is defaulted --
    // artwork only ever comes from a config file.
    airplay::OemButtonConfig oem_button{.enabled = true, .label = "Dashboard"};

    // --- Bring-up knobs -----------------------------------------------------
    // Not in the config file: these exist to take one layer at a time during a
    // hardware session, and a shipped vehicle wants the defaults.

    // Highest docs/carplay_bringup.md stage to attempt (2..7). Lower values stop
    // early, which keeps a failure at one layer from being masked by the noise
    // of the next one failing as a consequence.
    int max_stage = 7;

    // Where pair records and the accessory identity live. Empty selects a
    // default under the user's data directory.
    std::string state_dir;

    // Continue iAP2 identification without the MFi coprocessor. CarPlay will
    // not start, but everything below it can be exercised.
    bool allow_missing_mfi = false;

    // A fixed GPS fix for bench-testing the location uplink. When set it takes
    // precedence over any fix published on <prefix>/location.
    std::optional<LocationFix> static_location;
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
