// SPDX-License-Identifier: GPL-3.0-or-later

#include "airplay/screen_modes.h"

#include "plist/binary.h"

namespace airplay
{

namespace
{

// What each value rests on. See the header: none of these have been sent to a
// phone by this stack yet.
//
// resourceID 1 is the main screen. The /info `modes.resources` table we already
// send lists resources 1 and 2, and LIVI's modesChanged logging names 2 as main
// audio (LIVI cpStack.ts, "resourceID 2, entity: 1 = device/controller, 2 =
// accessory") -- leaving 1 as the screen.
constexpr int64_t kResourceMainScreen = 1;

// transferType take = 1 is what /info already sends, and the phone accepts that
// table. untake = 2 follows the numbering and is NOT confirmed.
constexpr int64_t kTransferTake = 1;
constexpr int64_t kTransferUntake = 2;

// The same priority and constraints /info sends: niceToHave, and "anytime" for
// the phone to take, borrow or return it. Anytime is what lets Siri or a call
// reclaim the screen while the car holds it, which is how the dashboard learns
// to switch back.
constexpr int64_t kPriorityNiceToHave = 100;
constexpr int64_t kConstraintAnytime = 100;

}  // namespace

Bytes buildChangeModesCommand(ScreenTransfer transfer)
{
    int64_t transfer_type = kTransferTake;
    switch (transfer)
    {
        case ScreenTransfer::take:
            transfer_type = kTransferTake;
            break;
        case ScreenTransfer::untake:
            transfer_type = kTransferUntake;
            break;
    }

    plist::Value screen = plist::Value::dict();
    screen.set("resourceID", plist::Value::integer(kResourceMainScreen));
    screen.set("transferType", plist::Value::integer(transfer_type));
    screen.set("transferPriority", plist::Value::integer(kPriorityNiceToHave));
    screen.set("takeConstraint", plist::Value::integer(kConstraintAnytime));
    screen.set("borrowConstraint", plist::Value::integer(kConstraintAnytime));
    screen.set("unborrowConstraint", plist::Value::integer(kConstraintAnytime));

    plist::Value params = plist::Value::dict();
    params.set("resources", plist::Value::array({std::move(screen)}));
    // Free text for the phone's own logs.
    params.set("reasonStr", plist::Value::string("redline dashboard page change"));

    plist::Value command = plist::Value::dict();
    command.set("type", plist::Value::string("changeModes"));
    command.set("params", std::move(params));
    return plist::encodeBinary(command);
}

Bytes buildRequestUiCommand(const std::optional<std::string>& url)
{
    // The phone's own press is `requestUI` with no url (isOemButtonPress); this
    // is the same shape in the other direction.
    plist::Value command = plist::Value::dict();
    command.set("type", plist::Value::string("requestUI"));
    if (url && !url->empty())
    {
        plist::Value params = plist::Value::dict();
        params.set("url", plist::Value::string(*url));
        command.set("params", std::move(params));
    }
    return plist::encodeBinary(command);
}

std::optional<ScreenEntity> parseScreenOwner(const plist::Value& modes_changed)
{
    if (!modes_changed.isDict())
    {
        return std::nullopt;
    }
    const plist::Value* params = modes_changed.find("params");
    if (params == nullptr || !params->isDict())
    {
        return std::nullopt;
    }
    const plist::Value* resources = params->find("resources");
    if (resources == nullptr || !resources->isArray())
    {
        return std::nullopt;
    }

    // The first screen entry decides. Two would be the phone contradicting
    // itself in one message, and the first is as good a reading as any.
    for (size_t i = 0; i < resources->size(); ++i)
    {
        const plist::Value& resource = resources->valueAt(i);
        if (!resource.isDict())
        {
            continue;
        }
        const plist::Value* id = resource.find("resourceID");
        if (id == nullptr || !id->isInteger() || id->asInteger() != kResourceMainScreen)
        {
            continue;
        }
        const plist::Value* entity = resource.find("entity");
        if (entity == nullptr || !entity->isInteger())
        {
            return std::nullopt;
        }
        switch (entity->asInteger())
        {
            case 0:
                return ScreenEntity::none;
            case 1:
                return ScreenEntity::controller;
            case 2:
                return ScreenEntity::accessory;
            default:
                // A value we have no name for is not an owner we can act on.
                return std::nullopt;
        }
    }
    return std::nullopt;
}

}  // namespace airplay
