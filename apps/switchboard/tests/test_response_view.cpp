// SPDX-License-Identifier: GPL-3.0-or-later
//
// ResponseView over hand-built results. No bus: everything here is about what a
// person reads -- that a service saying no (ok=false, or an error reply) cannot
// be missed among the other fields, and that Data reads as bytes.

#include "switchboard/response_view.h"

#include <spdlog/spdlog.h>

#include <QApplication>
#include <QTreeWidget>

#include <cstdio>
#include <string>

namespace
{

int failures = 0;
int checks = 0;

void expect(bool condition, const std::string& what)
{
    ++checks;
    if (!condition)
    {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

using Status = pub_sub::ServiceCallResult::Status;
using pub_sub::json;

pub_sub::ServiceCallResult replied(std::string schema, json value)
{
    pub_sub::ServiceCallResult result;
    result.status = Status::Replied;
    result.elapsed = std::chrono::milliseconds(42);
    pub_sub::ServiceReply reply;
    reply.schema = std::move(schema);
    reply.value = std::move(value);
    result.replies.push_back(std::move(reply));
    return result;
}

QTreeWidgetItem* findItem(QTreeWidget* tree, const QString& name)
{
    for (QTreeWidgetItemIterator it(tree); *it != nullptr; ++it)
    {
        if ((*it)->text(0) == name)
        {
            return *it;
        }
    }
    return nullptr;
}

}  // namespace

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    spdlog::set_level(spdlog::level::off);

    switchboard::ResponseView view;

    // ok = true: fields, no banner.
    view.showResult(replied("CanBridgeSetBitrateResponse",
                            {{"ok", true}, {"error", ""}, {"actualNominalBps", 500000}}));
    expect(view.statusText() == "✔ Replied · 42 ms", "a reply reads as replied, with its latency");
    expect(view.bannerText().isEmpty(), "ok = true raises no banner");
    auto* tree = view.findChild<QTreeWidget*>("response_tree_0");
    auto* bps = tree ? findItem(tree, "actualNominalBps") : nullptr;
    expect(bps != nullptr && bps->text(1) == "500000", "a field shows its value");
    expect(bps != nullptr && bps->text(2) == "UInt32", "and its type, read from the reply's schema");

    // ok = false: the reason is in a banner.
    view.showResult(replied("CanBridgeSetBitrateResponse",
                            {{"ok", false}, {"error", "channel 'can9' is not configured"}}));
    expect(view.bannerText().contains("channel 'can9' is not configured"),
           "ok = false puts the service's reason in the banner");

    // No `ok` field: the map services' shape, a status enum plus error text.
    // This is what map/route really answers when the graph file is missing.
    view.showResult(replied("MapRouteResponse", {{"status", "noSuchGraph"},
                                                 {"error", "not found: socal.graph"},
                                                 {"distanceM", 0.0}}));
    expect(view.bannerText().contains("status = noSuchGraph") &&
               view.bannerText().contains("not found: socal.graph"),
           "a failing status raises the banner with the status and the reason");

    view.showResult(replied("MapRouteResponse", {{"status", "noSuchGraph"}, {"error", ""}}));
    expect(view.bannerText().contains("status = noSuchGraph"),
           "a failing status alone is enough, with no error text");

    view.showResult(replied("MapRouteResponse", {{"status", "ok"}, {"error", "partial route"}}));
    expect(view.bannerText().contains("partial route"),
           "a non-empty error is enough, even beside status = ok");

    view.showResult(replied("MapRouteResponse", {{"status", "ok"}, {"error", ""}, {"distanceM", 12.5}}));
    expect(view.bannerText().isEmpty(), "status = ok with no error raises nothing");

    // An error reply.
    {
        pub_sub::ServiceCallResult result;
        result.status = Status::Replied;
        pub_sub::ServiceReply reply;
        reply.is_error = true;
        reply.error_text = "controller refused 123 bit/s";
        result.replies.push_back(reply);
        view.showResult(result);
        expect(view.statusText().startsWith("⚠ Service error"), "an error reply reads as an error");
        expect(view.bannerText().contains("controller refused 123 bit/s"), "with its text");
    }

    // Data, as grouped hex and as a truncated prefix.
    view.showResult(replied("Bd992SendCommandResponse", {{"ok", true}, {"replyData", "0102ff"}}));
    tree = view.findChild<QTreeWidget*>("response_tree_0");
    auto* data = tree ? findItem(tree, "replyData") : nullptr;
    expect(data != nullptr && data->text(1) == "01 02 ff", "Data reads as bytes");

    view.showResult(replied("Bd992SendCommandResponse",
                            {{"ok", true},
                             {"replyData", {{"_data_bytes", 9000}, {"hex_prefix", "0102"}}}}));
    tree = view.findChild<QTreeWidget*>("response_tree_0");
    data = tree ? findItem(tree, "replyData") : nullptr;
    expect(data != nullptr && data->text(1) == "9000 bytes: 0102…",
           "long Data reads as its length and a prefix");

    // Two replies: two sections.
    {
        auto result = replied("CanBridgeSetBitrateResponse", {{"ok", true}});
        result.replies.push_back(result.replies[0]);
        view.showResult(result);
        expect(view.replySectionCount() == 2, "two replies, two sections");
        expect(view.statusText().startsWith("✔ 2 replies"), "and the status says so");
    }

    // Nothing sent, nothing answered.
    {
        pub_sub::ServiceCallResult result;
        result.status = Status::RequestRejected;
        result.errors = {"nominalBps: expected a non-negative integer."};
        view.showResult(result);
        expect(view.statusText().contains("nothing was sent"), "a rejected request says nothing went");
        expect(view.bannerText().contains("nominalBps"), "and why");

        result.status = Status::NoReply;
        result.errors.clear();
        result.elapsed = std::chrono::milliseconds(2000);
        view.showResult(result);
        expect(view.statusText() == "✖ No reply after 2000 ms", "no reply names the wait");
    }

    std::fprintf(stderr, "%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
