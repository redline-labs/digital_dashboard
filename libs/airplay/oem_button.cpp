// SPDX-License-Identifier: GPL-3.0-or-later

#include "airplay/oem_button.h"

namespace airplay
{

void addOemButtonInfo(const OemButtonConfig& config, plist::Value& info)
{
    if (!config.enabled)
    {
        return;
    }

    info.set("oemIconVisible", plist::Value::boolean(true));
    if (!config.label.empty())
    {
        info.set("oemIconLabel", plist::Value::string(config.label));
    }

    std::vector<plist::Value> icons;
    icons.reserve(config.icons.size());
    for (const OemIcon& icon : config.icons)
    {
        plist::Value entry = plist::Value::dict();
        entry.set("imageData", plist::Value::data(icon.png));
        entry.set("widthPixels", plist::Value::integer(icon.width_px));
        entry.set("heightPixels", plist::Value::integer(icon.height_px));
        entry.set("prerendered", plist::Value::boolean(icon.prerendered));
        icons.push_back(std::move(entry));
    }
    // An empty array is worse than no key: it claims artwork exists and then
    // offers none, so leave the key out and let CarPlay use its placeholder.
    if (!icons.empty())
    {
        info.set("oemIcons", plist::Value::array(std::move(icons)));
    }
}

bool isOemButtonPress(const plist::Value& command)
{
    if (!command.isDict())
    {
        return false;
    }
    const plist::Value* type = command.find("type");
    if (type == nullptr || !type->isString() || type->asString() != "requestUI")
    {
        return false;
    }

    const plist::Value* params = command.find("params");
    if (params == nullptr || !params->isDict())
    {
        return true;  // no params at all: nothing to open, so it is the button
    }
    const plist::Value* url = params->find("url");
    return url == nullptr || !url->isString() || url->asString().empty();
}

}  // namespace airplay
