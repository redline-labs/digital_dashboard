// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the accessory is, as the phone sees it.
//
// Separate from receiver.h because this is what the capability declaration in
// GET /info is built from (info_plist.h), and that has to be reachable without
// dragging in the RTSP server, its threads and its sockets -- which is what
// makes the declaration testable.
#ifndef AIRPLAY_CONFIG_H_
#define AIRPLAY_CONFIG_H_

#include "airplay/oem_button.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace airplay
{

using Bytes = std::vector<uint8_t>;

// The main display's identity. The phone matches this string between the
// display entry in /info, the HID devices attached to it, and the keyframe
// requests aimed at it, so all three have to name it identically.
inline constexpr const char* kMainDisplayUuid = "b7e6c5a0-1111-4000-8000-000000000001";

// Which input device CarPlay should assume is the main one. It changes what
// CarPlay draws: Touch gets the touch-first UI, Knob gets a UI that can be
// walked with a rotary controller (focus rings, list navigation). Both HID
// devices are advertised either way -- this only says which to lay out for.
enum class PrimaryInput
{
    Touch = 1,
    Knob = 3,
};

struct ReceiverConfig
{
    // Address to bind. Empty binds to every interface, which is what the
    // bring-up wants; a link-local needs its scope id to bind specifically.
    std::string bind_address;
    uint16_t port = 7000;

    // Advertised in GET /info and used to derive pairing identity. `name` is
    // also signed over in both pairing handshakes, so it must match what
    // PairingSession was given.
    std::string name = "Dashboard";
    std::string model = "MercedesDashboard1,1";
    std::string manufacturer = "Dashboard";

    // Screen geometry advertised to the phone. Defaults match the carplay_demo
    // dashboard widget so the phone renders at the widget's aspect ratio.
    uint32_t width = 800;
    uint32_t height = 600;
    uint32_t fps = 30;

    // The panel's physical width in millimetres; the height is derived from it
    // and the pixel aspect ratio. CarPlay sizes text and touch targets from
    // this, so a wrong value gives a UI that is legible on a desk and not in a
    // car.
    uint32_t physical_width_mm = 200;

    // Which side the driver sits on. CarPlay mirrors its own layout for it.
    bool right_hand_drive = false;

    // Offer H.265 alongside H.264 and let the phone choose. The decode path
    // handles either -- nalu.cpp rewrites hvcC as well as avcC and knows HEVC's
    // different keyframe rule, and the codec travels on every packet -- so this
    // is purely an advertisement.
    //
    // Off by default. H.264 is the path with every hardware session behind it,
    // and turning this on hands the choice to the phone, which will take it.
    bool allow_hevc = false;

    // Advertised as both deviceID and macAddress in GET /info.
    std::string device_id = "02:00:00:00:00:01";

    // MFi coprocessor access for /auth-setup (MFiSAP). Left empty the receiver
    // answers 501, which stops the session: CarPlay will not proceed without a
    // genuine Apple authentication chip.
    std::function<Bytes()> mfi_certificate;
    std::function<Bytes(const Bytes& digest)> mfi_sign;
    // 2 => SHA-1/20-byte digests, 3 => SHA-256/32-byte.
    std::function<int()> mfi_protocol_major;

    // Where the accessory's AirPlay identity and its list of known phones are
    // kept. Empty means no persistence: a fresh identity every run, so every
    // phone pairs from scratch and pair-verify cannot be enforced.
    std::string state_dir;

    // The manufacturer button on CarPlay's home screen. Disabled by default.
    OemButtonConfig oem_button;

    PrimaryInput primary_input = PrimaryInput::Touch;
};

}  // namespace airplay

#endif  // AIRPLAY_CONFIG_H_
