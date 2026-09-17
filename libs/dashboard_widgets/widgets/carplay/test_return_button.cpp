// SPDX-License-Identifier: GPL-3.0-or-later
//
// The CarPlay widget's way off its page, and what it tells the driver about
// being on screen -- through a real widget and a real bus.
//
// The policy has its own truth-table test; this is what proves the policy is
// wired: that the button actually appears over a widget with no session, that a
// live session actually removes it, that a driver going silent actually brings
// it back, that a tap actually sends ONE command, and that hiding the widget
// actually stops video.
#include "carplay/carplay_widget.h"

#include "dashboard_pages.capnp.h"
#include "pub_sub/zenoh_publisher.h"
#include "pub_sub/zenoh_subscriber.h"

#include <QApplication>
#include <QImage>
#include <QPushButton>

#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

using namespace std::chrono_literals;

namespace
{

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const std::string& what)
{
    ++g_checks;
    if (!condition)
    {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

void pump(std::chrono::milliseconds duration)
{
    const auto deadline = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < deadline)
    {
        QApplication::processEvents();
        std::this_thread::sleep_for(2ms);
    }
}

// Pumps until `condition` holds or `limit` passes. Returns whether it held.
template <typename Condition>
bool pumpUntil(Condition condition, std::chrono::milliseconds limit)
{
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (condition())
        {
            return true;
        }
        QApplication::processEvents();
        std::this_thread::sleep_for(2ms);
    }
    return condition();
}

bool buttonShown(const CarPlayWidget& widget)
{
    return widget.returnButton() != nullptr && widget.returnButton()->isVisible();
}

}  // namespace

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    spdlog::set_level(spdlog::level::warn);

    // Unique keys, so a real driver or another test on the same bus cannot
    // answer for this one.
    const std::string prefix = "test/carplay_return_" + std::to_string(::getpid());

    CarplayConfig_t cfg;
    cfg.video_key = prefix + "/video";
    cfg.audio_key = prefix + "/audio";
    cfg.mic_key = prefix + "/mic";
    cfg.input_key = prefix + "/input";
    cfg.session_key = prefix + "/session";
    cfg.visibility_key = prefix + "/visibility";
    cfg.session_stale_after_ms = 300;
    cfg.return_button.enabled = true;
    cfg.return_button.label = "Vehicle";
    cfg.return_button.command.target = "test_pages";
    cfg.return_button.command.action = page_action_t::go_to;
    cfg.return_button.command.page = "vehicle";

    std::mutex mutex;
    std::vector<std::pair<PageStackCommand::Action, std::string>> commands;
    std::vector<bool> visibility;

    pub_sub::ZenohTypedSubscriber<PageStackCommand> command_sub(
        "dashboard/pages/test_pages/command",
        [&](PageStackCommand::Reader reader) {
            const std::lock_guard<std::mutex> lock(mutex);
            commands.emplace_back(reader.getAction(), reader.getPage().cStr());
        });
    pub_sub::ZenohTypedSubscriber<CarPlayVisibility> visibility_sub(
        cfg.visibility_key,
        [&](CarPlayVisibility::Reader reader) {
            const std::lock_guard<std::mutex> lock(mutex);
            visibility.push_back(reader.getVisible());
        });
    pub_sub::ZenohPublisher<CarPlaySessionState> session_pub(cfg.session_key);

    // A parent standing in for a page, so hiding it is what a page switch does.
    QWidget page;
    page.resize(800, 600);
    auto* widget = new CarPlayWidget(cfg, &page);
    widget->setObjectName("carplay_view");
    widget->setGeometry(0, 0, 800, 600);
    page.show();
    pump(300ms);  // let zenoh match the publishers and subscribers

    check(widget->returnButton() != nullptr, "an enabled return button exists");
    check(widget->returnButton() && widget->returnButton()->objectName() == "carplay_view:return",
          "and is addressable as <widget>:return");
    check(buttonShown(*widget), "with no session at all, the return button shows");
    const QImage without_session = page.grab().toImage();

    // A live session removes it.
    const auto publishLive = [&](bool connected, CarPlaySessionState::Phase phase) {
        session_pub.fields().setDeviceConnected(connected);
        session_pub.fields().setPhase(phase);
        session_pub.put();
    };
    publishLive(true, CarPlaySessionState::Phase::RECORDING);
    check(pumpUntil([&] { return !buttonShown(*widget); }, 1000ms), "a live session hides the return button");
    check(page.grab().toImage() != without_session, "and the picture changes with it");

    // Connected but not yet recording is not live.
    publishLive(true, CarPlaySessionState::Phase::AIRPLAY_HANDSHAKE);
    check(pumpUntil([&] { return buttonShown(*widget); }, 1000ms), "a session still connecting shows the button");

    // A driver that dies mid-session leaves "recording" as its last word.
    publishLive(true, CarPlaySessionState::Phase::RECORDING);
    check(pumpUntil([&] { return !buttonShown(*widget); }, 1000ms), "live again hides it");
    check(pumpUntil([&] { return buttonShown(*widget); }, 2000ms),
          "session state going silent brings the button back");

    // A tap sends exactly one command.
    {
        const std::lock_guard<std::mutex> lock(mutex);
        commands.clear();
    }
    widget->returnButton()->click();
    pumpUntil([&] { const std::lock_guard<std::mutex> lock(mutex); return !commands.empty(); }, 1000ms);
    pump(200ms);
    {
        const std::lock_guard<std::mutex> lock(mutex);
        check(commands.size() == 1, "one tap sends one command, got " + std::to_string(commands.size()));
        check(!commands.empty() && commands[0].first == PageStackCommand::Action::GO_TO &&
                  commands[0].second == "vehicle",
              "the command is the configured go_to vehicle");
    }

    // Visibility: reported, and video follows it.
    check(widget->reportsVisible() && widget->videoSubscribed(), "a shown widget subscribes to video");
    {
        const std::lock_guard<std::mutex> lock(mutex);
        visibility.clear();
    }
    page.hide();
    check(pumpUntil([&] { return !widget->videoSubscribed(); }, 1000ms), "hiding the page drops the video subscription");
    check(pumpUntil([&] { const std::lock_guard<std::mutex> lock(mutex); return !visibility.empty() && !visibility.back(); },
                    1000ms),
          "and reports visible: false");
    page.show();
    check(pumpUntil([&] { return widget->videoSubscribed(); }, 1000ms), "showing it again subscribes to video");
    check(pumpUntil([&] { const std::lock_guard<std::mutex> lock(mutex); return !visibility.empty() && visibility.back(); },
                    1000ms),
          "and reports visible: true");

    // A disabled button is never built.
    CarplayConfig_t plain = cfg;
    plain.return_button.enabled = false;
    plain.visibility_key = prefix + "/visibility_plain";
    CarPlayWidget no_button(plain);
    check(no_button.returnButton() == nullptr, "a disabled return button is not built");

    delete widget;
    std::fprintf(stderr, "%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
