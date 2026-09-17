// SPDX-License-Identifier: GPL-3.0-or-later
//
// The screen-ownership messages: what we send, and what we read back.
//
// SYNTHETIC. The modesChanged bodies below are written from the key names we
// believe the phone uses (see screen_modes.cpp for the evidence), not captured
// from one. When a capture exists, it replaces them -- a fixture authored from
// the same reading as the parser agrees with the parser exactly where both are
// wrong.
#include "airplay/screen_modes.h"

#include "plist/binary.h"

#include <spdlog/spdlog.h>

#include <cstdio>
#include <string>
#include <vector>

namespace
{

int failures = 0;

void expect(bool condition, const std::string& what)
{
    if (!condition)
    {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++failures;
    }
}

plist::Value decode(const airplay::Bytes& body)
{
    auto value = plist::decodeBinary(body);
    return value ? *value : plist::Value::dict();
}

int64_t intAt(const plist::Value& dict, const char* key)
{
    const plist::Value* v = dict.find(key);
    return (v != nullptr && v->isInteger()) ? v->asInteger() : -999;
}

plist::Value resource(int64_t id, int64_t entity)
{
    plist::Value r = plist::Value::dict();
    r.set("resourceID", plist::Value::integer(id));
    r.set("entity", plist::Value::integer(entity));
    return r;
}

plist::Value modesChanged(std::vector<plist::Value> resources)
{
    plist::Value params = plist::Value::dict();
    params.set("resources", plist::Value::array(std::move(resources)));
    plist::Value command = plist::Value::dict();
    command.set("type", plist::Value::string("modesChanged"));
    command.set("params", std::move(params));
    return command;
}

void testChangeModes()
{
    for (const auto transfer : {airplay::ScreenTransfer::take, airplay::ScreenTransfer::untake})
    {
        const bool take = transfer == airplay::ScreenTransfer::take;
        const plist::Value command = decode(airplay::buildChangeModesCommand(transfer));
        const plist::Value* type = command.find("type");
        expect(type != nullptr && type->isString() && type->asString() == "changeModes", "type is changeModes");

        const plist::Value* params = command.find("params");
        const plist::Value* resources = params ? params->find("resources") : nullptr;
        expect(resources != nullptr && resources->isArray() && resources->size() == 1,
               "one resource entry, and only the screen");
        if (resources == nullptr || !resources->isArray() || resources->size() != 1)
        {
            continue;
        }
        const plist::Value& screen = resources->valueAt(0);
        expect(intAt(screen, "resourceID") == 1, "the resource is the main screen, 1");
        expect(intAt(screen, "transferType") == (take ? 1 : 2), take ? "take is 1" : "untake is 2");
        expect(intAt(screen, "transferPriority") == 100, "priority niceToHave, as /info sends");
        expect(intAt(screen, "takeConstraint") == 100 && intAt(screen, "borrowConstraint") == 100 &&
                   intAt(screen, "unborrowConstraint") == 100,
               "the phone may reclaim it any time");
    }
}

void testRequestUi()
{
    const plist::Value bare = decode(airplay::buildRequestUiCommand(std::nullopt));
    expect(bare.find("type") && bare.find("type")->asString() == "requestUI", "requestUI type");
    expect(bare.find("params") == nullptr, "no url means no params, the same shape as the phone's own press");

    const plist::Value empty = decode(airplay::buildRequestUiCommand(std::string()));
    expect(empty.find("params") == nullptr, "an empty url is no url");

    const plist::Value with_url = decode(airplay::buildRequestUiCommand(std::string("maps:")));
    const plist::Value* params = with_url.find("params");
    const plist::Value* url = params ? params->find("url") : nullptr;
    expect(url != nullptr && url->isString() && url->asString() == "maps:", "a url is carried in params.url");
}

void testParseScreenOwner()
{
    using airplay::parseScreenOwner;
    using airplay::ScreenEntity;

    expect(parseScreenOwner(modesChanged({resource(2, 2), resource(1, 1)})) == ScreenEntity::controller,
           "the screen entry is found among others: owned by the phone");
    expect(parseScreenOwner(modesChanged({resource(1, 2)})) == ScreenEntity::accessory, "owned by the car");
    expect(parseScreenOwner(modesChanged({resource(1, 0)})) == ScreenEntity::none, "owned by no one");
    expect(parseScreenOwner(modesChanged({resource(1, 2), resource(1, 1)})) == ScreenEntity::accessory,
           "two screen entries: the first decides");

    // Everything malformed is "does not say", never a guess.
    expect(!parseScreenOwner(modesChanged({resource(2, 1)})), "audio only says nothing about the screen");
    expect(!parseScreenOwner(modesChanged({})), "an empty resources list says nothing");
    expect(!parseScreenOwner(modesChanged({resource(1, 7)})), "an entity with no name is not an owner");
    expect(!parseScreenOwner(plist::Value::string("nope")), "not a dict");
    expect(!parseScreenOwner(plist::Value::dict()), "no params");

    plist::Value params_not_dict = plist::Value::dict();
    params_not_dict.set("params", plist::Value::integer(3));
    expect(!parseScreenOwner(params_not_dict), "params not a dict");

    plist::Value resources_not_array = plist::Value::dict();
    plist::Value p = plist::Value::dict();
    p.set("resources", plist::Value::dict());
    resources_not_array.set("params", std::move(p));
    expect(!parseScreenOwner(resources_not_array), "resources not an array");

    plist::Value no_entity = plist::Value::dict();
    no_entity.set("resourceID", plist::Value::integer(1));
    expect(!parseScreenOwner(modesChanged({no_entity})), "a screen entry with no entity");

    plist::Value string_entity = plist::Value::dict();
    string_entity.set("resourceID", plist::Value::integer(1));
    string_entity.set("entity", plist::Value::string("1"));
    expect(!parseScreenOwner(modesChanged({string_entity})), "an entity that is not an integer");

    plist::Value string_id = plist::Value::dict();
    string_id.set("resourceID", plist::Value::string("1"));
    string_id.set("entity", plist::Value::integer(1));
    expect(!parseScreenOwner(modesChanged({string_id, plist::Value::integer(5)})),
           "a resourceID that is not an integer, and an entry that is not a dict, are skipped");

    // Our own modes table from /info is the other shape this could meet; it has
    // no `entity`, so it must not read as an owner.
    plist::Value appstates_only = plist::Value::dict();
    plist::Value ap = plist::Value::dict();
    ap.set("appStates", plist::Value::array({}));
    appstates_only.set("params", std::move(ap));
    expect(!parseScreenOwner(appstates_only), "a modesChanged about app states only");
}

}  // namespace

int main()
{
    spdlog::set_level(spdlog::level::off);
    testChangeModes();
    testRequestUi();
    testParseScreenOwner();
    std::fprintf(stderr, "%d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
