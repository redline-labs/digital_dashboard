// SPDX-License-Identifier: GPL-3.0-or-later
//
// The manufacturer button, both directions: what GET /info advertises, and how
// the phone's press is told apart from the other traffic on the event channel.
#include "airplay/oem_button.h"

#include "plist/binary.h"

#include <spdlog/spdlog.h>

#include <cstdlib>

namespace
{

int failures = 0;

void expect(bool condition, const char* what)
{
    if (!condition)
    {
        SPDLOG_ERROR("FAIL: {}", what);
        ++failures;
    }
}

airplay::OemIcon makeIcon(uint32_t size, bool prerendered = true)
{
    airplay::OemIcon icon;
    icon.png.assign(size, 0x7f);  // stand-in bytes; nothing here decodes them
    icon.width_px = size;
    icon.height_px = size;
    icon.prerendered = prerendered;
    return icon;
}

// One decoded POST /command body, built the way the phone would send it.
plist::Value command(const char* type)
{
    plist::Value value = plist::Value::dict();
    value.set("type", plist::Value::string(type));
    return value;
}

plist::Value requestUiWithUrl(const char* url)
{
    plist::Value params = plist::Value::dict();
    params.set("url", plist::Value::string(url));
    plist::Value value = command("requestUI");
    value.set("params", std::move(params));
    return value;
}

}  // namespace

int main()
{
    using airplay::addOemButtonInfo;
    using airplay::isOemButtonPress;
    using airplay::OemButtonConfig;

    // Disabled: not one key, so the phone sees a vehicle with no such button.
    {
        plist::Value info = plist::Value::dict();
        OemButtonConfig config;
        config.label = "Ignored";
        config.icons.push_back(makeIcon(60));
        addOemButtonInfo(config, info);
        expect(info.size() == 0, "disabled adds nothing");
    }

    // Enabled with artwork: visibility, label and one entry per rendition.
    {
        plist::Value info = plist::Value::dict();
        OemButtonConfig config;
        config.enabled = true;
        config.label = "Dashboard";
        config.icons.push_back(makeIcon(60));
        config.icons.push_back(makeIcon(120, false));
        addOemButtonInfo(config, info);

        const plist::Value* visible = info.find("oemIconVisible");
        expect(visible != nullptr && visible->isBool() && visible->asBool(),
               "oemIconVisible is true");

        const plist::Value* label = info.find("oemIconLabel");
        expect(label != nullptr && label->asString() == "Dashboard", "oemIconLabel carries label");

        const plist::Value* icons = info.find("oemIcons");
        expect(icons != nullptr && icons->isArray() && icons->size() == 2, "one entry per icon");
        if (icons != nullptr && icons->isArray() && icons->size() == 2)
        {
            const plist::Value& first = icons->at(0);
            expect(first.find("widthPixels")->asInteger() == 60, "icon width");
            expect(first.find("heightPixels")->asInteger() == 60, "icon height");
            expect(first.find("imageData")->isData() && first.find("imageData")->asData().size() == 60,
                   "icon data is passed through verbatim");
            expect(first.find("prerendered")->asBool(), "prerendered defaults TRUE");
            expect(!icons->at(1).find("prerendered")->asBool(), "and is per icon");
        }

        // The whole point of building this is that it survives the wire format
        // the phone actually reads it from.
        const auto decoded = plist::decodeBinary(plist::encodeBinary(info));
        expect(decoded && *decoded == info, "info keys survive a binary plist round trip");
    }

    // Enabled with no label and no artwork: the keys that would lie are absent.
    {
        plist::Value info = plist::Value::dict();
        OemButtonConfig config;
        config.enabled = true;
        addOemButtonInfo(config, info);
        expect(info.find("oemIconVisible") != nullptr, "visibility still advertised");
        expect(info.find("oemIconLabel") == nullptr, "empty label omitted");
        expect(info.find("oemIcons") == nullptr, "empty icon list omitted");
    }

    // prerendered must default to true, and this is not a style preference.
    //
    // Hardware, 2026-08-02, iPhone17,1 / AirPlay 950.7.1: an icon advertised
    // with prerendered=false renders as an EMPTY TILE. The label shows, the
    // artwork does not. Sending the identical PNG with prerendered=true renders
    // it correctly. Both directions were tried against the phone.
    //
    // The name misleads -- it reads as "should CarPlay apply its own corner
    // mask and shine" -- so a future reader may well flip it back on the theory
    // that a masked icon looks more native. This test is here to stop that.
    {
        airplay::OemIcon fresh;
        expect(fresh.prerendered, "a default-constructed icon is prerendered");

        OemButtonConfig config;
        config.enabled = true;
        config.icons.push_back(airplay::OemIcon{});
        plist::Value info = plist::Value::dict();
        addOemButtonInfo(config, info);
        const plist::Value* icons = info.find("oemIcons");
        expect(icons != nullptr && icons->isArray() && icons->size() == 1, "one icon");
        if (icons != nullptr && icons->isArray() && icons->size() == 1)
        {
            const plist::Value* flag = icons->at(0).find("prerendered");
            expect(flag != nullptr && flag->isBool() && flag->asBool(),
                   "and it reaches /info as true -- false renders an empty tile on hardware");
        }
    }

    // The press: requestUI with nothing to open.
    {
        expect(isOemButtonPress(command("requestUI")), "requestUI with no params is the button");

        plist::Value empty_params = command("requestUI");
        empty_params.set("params", plist::Value::dict());
        expect(isOemButtonPress(empty_params), "requestUI with empty params is the button");

        expect(isOemButtonPress(requestUiWithUrl("")), "requestUI with an empty url is the button");
    }

    // Everything else on the event channel is not the button.
    {
        expect(!isOemButtonPress(requestUiWithUrl("maps://")),
               "requestUI naming a url is an app request, not the button");
        expect(!isOemButtonPress(command("duckAudio")), "another command type is not the button");
        expect(!isOemButtonPress(plist::Value::dict()), "a body with no type is not the button");
        expect(!isOemButtonPress(plist::Value::string("requestUI")),
               "a non-dict body is not the button");
    }

    if (failures == 0)
    {
        SPDLOG_INFO("oem_button tests passed");
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
}
