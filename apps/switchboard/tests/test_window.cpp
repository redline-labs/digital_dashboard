// SPDX-License-Identifier: GPL-3.0-or-later
//
// The whole window against in-process services, the way a person uses it:
// a service appears, gets selected, the form is filled in, Submit, the reply
// shows, and the history puts an old request back.
//
// The services share this process's zenoh session, which is how zenoh routes a
// local query to a local queryable -- so this is `net` as well as `gui`.

#include "switchboard/switchboard_window.h"

#include "pub_sub/zenoh_service.h"

#include "bd992.capnp.h"
#include "can_bridge.capnp.h"

#include <spdlog/spdlog.h>

#include <QAbstractEventDispatcher>
#include <QApplication>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>

#include <chrono>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>

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

// Spins the event loop until `done`, since both discovery and replies arrive
// through it.
bool waitFor(const std::function<bool()>& done,
             std::chrono::milliseconds timeout = std::chrono::seconds(5))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        QAbstractEventDispatcher::instance()->processEvents(QEventLoop::AllEvents);
        if (done())
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return done();
}

using switchboard::json;

constexpr const char* kEchoKey = "test/switchboard/echo";
constexpr const char* kThrowsKey = "test/switchboard/throws";

}  // namespace

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    spdlog::set_level(spdlog::level::off);

    if (!pub_sub::SessionManager::getOrCreate())
    {
        std::fprintf(stderr, "No zenoh session available; cannot run.\n");
        return 1;
    }

    pub_sub::ZenohService<Bd992SendCommandRequest, Bd992SendCommandResponse> echo(
        kEchoKey,
        [](const Bd992SendCommandRequest::Reader& request, Bd992SendCommandResponse::Builder& response)
        {
            response.setOk(true);
            response.setReplyType(request.getPacketType());
            response.setReplyData(request.getData());
        });

    pub_sub::ZenohService<CanBridgeSetBitrateRequest, CanBridgeSetBitrateResponse> throws(
        kThrowsKey,
        [](const CanBridgeSetBitrateRequest::Reader&, CanBridgeSetBitrateResponse::Builder&)
        { throw std::runtime_error("controller refused the rate"); });

    {
        switchboard::SwitchboardWindow window(2000);
        window.show();

        const auto listed = [&window](const std::string& key)
        {
            window.serviceList().refresh();
            for (const auto& row : window.serviceList().rows())
            {
                if (row.key == key && row.reachable)
                {
                    return true;
                }
            }
            return false;
        };
        expect(waitFor([&] { return listed(kEchoKey) && listed(kThrowsKey); }),
               "in-process services appear in the list");

        std::string error;
        expect(!window.selectService("test/switchboard/nothing", "", error) && !error.empty(),
               "selecting an unknown service is refused with a reason");
        expect(window.submit(std::nullopt, error) < 0, "submitting with nothing selected is refused");

        // Select, fill in, submit, read.
        expect(window.selectService(kEchoKey, "", error), "a listed service can be selected");
        expect(window.form().hasSchema(), "and gets a form");
        auto* title = window.findChild<QLabel*>("service_title");
        expect(title != nullptr && title->text() == kEchoKey, "whose header names the service");

        std::vector<std::string> errors;
        expect(window.form().setValue({{"packetType", 86}, {"data", "01 02"}}, errors),
               "the form takes the request");

        const int call = window.submit(std::nullopt, error);
        expect(call > 0, "submit returns a call id: " + error);
        expect(window.callInFlight(), "and the call is in flight");
        auto* submit_button = window.findChild<QPushButton*>("submit_button");
        expect(submit_button != nullptr && !submit_button->isEnabled(),
               "Submit is disabled while it is");
        expect(window.submit(std::nullopt, error) < 0, "a second submit is refused meanwhile");

        expect(waitFor([&] { return !window.callInFlight(); }), "the call completes");
        expect(window.response().statusText().startsWith("✔ Replied"), "the reply is shown");
        const switchboard::CallRecord* record = window.history().find(call);
        expect(record != nullptr && record->complete, "and recorded");
        expect(record != nullptr && record->result.replies.size() == 1 &&
                   record->result.replies[0].value.value("replyData", std::string()) == "0102",
               "with the service's answer, Data round-tripped as hex");
        expect(submit_button->isEnabled(), "Submit is enabled again");

        // A service whose handler throws answers with its reason.
        expect(window.selectService(kThrowsKey, "", error), "the throwing service can be selected");
        errors.clear();
        window.form().setValue({{"channel", "can0"}, {"nominalBps", 1}}, errors);
        expect(window.submit(std::nullopt, error) > 0, "and called");
        expect(waitFor([&] { return !window.callInFlight(); }), "that call completes too");
        expect(window.response().bannerText().contains("controller refused the rate"),
               "and the handler's exception is what the banner says");

        // Back to the first service: its last request comes back, and a history
        // click restores an older one after the form has changed.
        expect(window.selectService(kEchoKey, "", error), "the first service can be reselected");
        expect(window.form().value()["packetType"] == 86, "its last request is back in the form");

        errors.clear();
        window.form().setValue({{"packetType", 1}}, errors);
        auto* list = window.findChild<QListWidget*>("history_list");
        expect(list != nullptr && list->count() == 1, "its history lists the one call");
        if (list != nullptr && list->count() == 1)
        {
            emit list->itemClicked(list->item(0));
        }
        expect(window.form().value()["packetType"] == 86, "clicking it restores that request");
    }

    pub_sub::SessionManager::shutdown();

    std::fprintf(stderr, "%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
