// SPDX-License-Identifier: GPL-3.0-or-later
//
// The GET /info capability declaration.
//
// This message has the worst failure signature in the stack: the phone does not
// reject a wrong one. It accepts the session, then tears it down without ever
// asking for a stream, which looks exactly like a transport fault several
// layers below. So what these tests hold is the shape of the declaration --
// that the keys the phone actually reads are present, spelled the way it
// expects, and consistent with each other.
//
// They cannot prove the declaration is *correct* (only a phone can), which is
// the point: they stop it changing by accident.
#include "airplay/info_plist.h"

#include "airplay/hid.h"
#include "plist/binary.h"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <set>
#include <string>

namespace
{

int failures = 0;

void expect(bool condition, const std::string& what)
{
    if (!condition)
    {
        SPDLOG_ERROR("FAIL: {}", what);
        ++failures;
    }
}

const plist::Value* at(const plist::Value& parent, const std::string& key)
{
    return parent.isDict() ? parent.find(key) : nullptr;
}

bool hasInteger(const plist::Value& parent, const std::string& key, int64_t expected)
{
    const plist::Value* value = at(parent, key);
    return value != nullptr && value->isInteger() && value->asInteger() == expected;
}

airplay::ReceiverConfig makeConfig()
{
    airplay::ReceiverConfig config;
    config.width = 1280;
    config.height = 720;
    config.fps = 30;
    config.name = "Test Dashboard";
    config.model = "TestModel1,1";
    config.device_id = "02:00:00:00:00:09";
    return config;
}

}  // namespace

int main()
{
    using airplay::buildInfoPlist;

    const airplay::ReceiverConfig config = makeConfig();
    const plist::Value info = buildInfoPlist(config);
    expect(info.isDict(), "/info is a dict");

    // The top-level keys the phone reads. Named individually rather than
    // counted, so adding one is not a test failure but losing one is.
    {
        for (const char* key : {"sourceVersion", "features", "statusFlags", "model",
                                "manufacturer", "deviceID", "bluetoothIDs", "name",
                                "rightHandDrive", "keepAliveLowPower", "modes",
                                "extendedFeatures", "displays", "hidDevices",
                                "audioLatencies", "audioFormats"})
        {
            expect(at(info, key) != nullptr, std::string("/info carries ") + key);
        }
        const plist::Value* model = at(info, "model");
        expect(model != nullptr && model->asString() == config.model, "model comes from config");
        const plist::Value* name = at(info, "name");
        expect(name != nullptr && name->asString() == config.name, "name comes from config");
    }

    // The display entry. The spelling here is the classic silent failure: the
    // phone wants widthPixels/heightPixels and a stream type, not width/height.
    const plist::Value* displays = at(info, "displays");
    expect(displays != nullptr && displays->isArray() && displays->size() == 1,
           "exactly one display is advertised");
    if (displays != nullptr && displays->isArray() && displays->size() == 1)
    {
        const plist::Value& display = displays->at(0);
        expect(hasInteger(display, "widthPixels", config.width), "display width is widthPixels");
        expect(hasInteger(display, "heightPixels", config.height), "display height is heightPixels");
        expect(at(display, "width") == nullptr && at(display, "height") == nullptr,
               "and not the bare width/height the phone ignores");
        expect(hasInteger(display, "type", 110), "display carries its stream type");
        expect(hasInteger(display, "maxFPS", config.fps), "display carries the frame rate");
        expect(hasInteger(display, "primaryInputDevice",
                          static_cast<int64_t>(airplay::PrimaryInput::Touch)),
               "primaryInputDevice follows the config");

        // Physical size is derived, and must keep the pixel aspect ratio or the
        // phone renders at the wrong shape.
        const plist::Value* width_physical = at(display, "widthPhysical");
        const plist::Value* height_physical = at(display, "heightPhysical");
        expect(width_physical != nullptr && height_physical != nullptr, "physical size is declared");
        if (width_physical != nullptr && height_physical != nullptr)
        {
            const double pixel_ratio = static_cast<double>(config.width) / config.height;
            const double physical_ratio =
                static_cast<double>(width_physical->asInteger()) / height_physical->asInteger();
            expect(std::abs(pixel_ratio - physical_ratio) < 0.05,
                   "physical size keeps the pixel aspect ratio");
        }

        // We claim "viewAreas" in enabledFeatures at SETUP, so supplying none
        // here would leave the phone unable to lay anything out.
        const plist::Value* areas = at(display, "viewAreas");
        expect(areas != nullptr && areas->isArray() && areas->size() == 1, "one view area");
        if (areas != nullptr && areas->isArray() && areas->size() == 1)
        {
            const plist::Value& area = areas->at(0);
            expect(hasInteger(area, "widthPixels", config.width), "view area spans the display");
            expect(at(area, "safeArea") != nullptr, "and carries a safe area");
        }
        expect(at(display, "initialViewArea") != nullptr, "an initial view area is named");
    }

    // Every input device, each with the uuid the reports will name. A device
    // whose /info uuid does not match its report uuid is silently ignored.
    const plist::Value* devices = at(info, "hidDevices");
    expect(devices != nullptr && devices->isArray() && devices->size() == 4,
           "touch, knob, media and telephony are all advertised");
    if (devices != nullptr && devices->isArray())
    {
        std::set<std::string> uuids;
        bool descriptors = true;
        bool attached = true;
        for (size_t i = 0; i < devices->size(); ++i)
        {
            const plist::Value& device = devices->at(i);
            const plist::Value* uuid = at(device, "uuid");
            if (uuid != nullptr)
            {
                uuids.insert(uuid->asString());
            }
            const plist::Value* descriptor = at(device, "hidDescriptor");
            descriptors = descriptors && descriptor != nullptr && descriptor->isData() &&
                          !descriptor->asData().empty();
            const plist::Value* display_uuid = at(device, "displayUUID");
            attached = attached && display_uuid != nullptr &&
                       display_uuid->asString() == airplay::kMainDisplayUuid;
        }
        expect(uuids.size() == 4, "the four devices have distinct uuids");
        expect(descriptors, "each device carries a non-empty report descriptor");
        expect(attached, "each device is attached to the main display");

        for (uint32_t uid : {airplay::hid::kTouchUid, airplay::hid::kKnobUid,
                             airplay::hid::kMediaUid, airplay::hid::kTelephonyUid})
        {
            expect(uuids.count(airplay::hid::uidToString(uid)) == 1,
                   "advertised uuid " + airplay::hid::uidToString(uid) +
                       " matches what reports will name");
        }
    }

    // Resource arbitration. A phone that sees no `modes` does not start.
    const plist::Value* modes = at(info, "modes");
    if (modes != nullptr)
    {
        const plist::Value* resources = at(*modes, "resources");
        const plist::Value* app_states = at(*modes, "appStates");
        expect(resources != nullptr && resources->isArray() && resources->size() == 2,
               "the screen and audio resources are both offered");
        expect(app_states != nullptr && app_states->isArray() && app_states->size() == 3,
               "call, speech and turn-by-turn app states are declared");
    }

    // Audio. A head unit that declares none may get no streams at all.
    const plist::Value* formats = at(info, "audioFormats");
    const plist::Value* latencies = at(info, "audioLatencies");
    expect(formats != nullptr && formats->isArray() && formats->size() > 0, "audio formats exist");
    expect(latencies != nullptr && latencies->isArray() && latencies->size() > 0,
           "audio latencies exist");
    if (formats != nullptr && formats->isArray())
    {
        bool spelled = true;
        for (size_t i = 0; i < formats->size(); ++i)
        {
            // audioOutputFormats, not outputFormats: the short spelling is
            // accepted into the plist and read by nobody.
            spelled = spelled && at(formats->at(i), "audioOutputFormats") != nullptr &&
                      at(formats->at(i), "audioType") != nullptr;
        }
        expect(spelled, "every audio format names audioType and audioOutputFormats");
    }

    // A knob-primary head unit changes one field and nothing else.
    {
        airplay::ReceiverConfig knob = makeConfig();
        knob.primary_input = airplay::PrimaryInput::Knob;
        const plist::Value knob_info = buildInfoPlist(knob);
        const plist::Value* knob_displays = at(knob_info, "displays");
        expect(knob_displays != nullptr && knob_displays->isArray() &&
                   hasInteger(knob_displays->at(0), "primaryInputDevice", 3),
               "a knob-primary config advertises primaryInputDevice 3");
        expect(at(knob_info, "hidDevices")->size() == 4,
               "and still advertises every input device");
    }

    // HEVC is offered only when asked for. Both halves of the offer matter --
    // this is the /info half; the SETUP half is enabledFeatures.
    {
        expect(at(info, "hevcInfo") == nullptr, "H.265 is not offered by default");

        airplay::ReceiverConfig hevc = makeConfig();
        hevc.allow_hevc = true;
        const plist::Value* offered = at(buildInfoPlist(hevc), "hevcInfo");
        expect(offered != nullptr, "and is offered when enabled");
        expect(offered != nullptr && offered->isDict(),
               "as a dict -- presence is the offer, it carries no parameters");
    }

    // The manufacturer button is off unless asked for (see test_oem_button for
    // the keys themselves); this is the wiring into /info.
    {
        expect(at(info, "oemIconVisible") == nullptr, "no manufacturer button by default");

        airplay::ReceiverConfig oem = makeConfig();
        oem.oem_button.enabled = true;
        oem.oem_button.label = "Vehicle";
        expect(at(buildInfoPlist(oem), "oemIconVisible") != nullptr,
               "an enabled button reaches /info");
    }

    // Everything above is only true if it survives the wire format the phone
    // reads it from.
    {
        const auto encoded = plist::encodeBinary(info);
        const auto decoded = plist::decodeBinary(encoded);
        expect(decoded.has_value() && *decoded == info, "/info survives a binary plist round trip");
    }

    if (failures == 0)
    {
        SPDLOG_INFO("info plist tests passed");
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
}
