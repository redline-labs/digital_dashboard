// SPDX-License-Identifier: GPL-3.0-or-later
// Multitouch through a real widget and a real zenoh subscriber: what a pinch,
// a third finger, a tap on the return button and a page switch mid-touch put
// on the input topic.
//
// Touch goes in through QTest::touchEvent, which delivers the way a
// touchscreen does -- through the window system and Qt's own routing -- rather
// than handing the widget a QTouchEvent directly. That is the point: the
// return button case depends on Qt offering a declined touch back as mouse
// events, which only its real delivery path does, and a hand-built
// QEventPoint does not even carry a widget-local position.
#include "carplay/carplay_widget.h"

#include "pub_sub/zenoh_subscriber.h"

#include <QApplication>
#include <QPushButton>
#include <QtGui/QPointingDevice>
#include <QtTest/QTest>

#include <spdlog/spdlog.h>

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

void check(bool condition, const std::string& what)
{
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

struct Contact
{
    unsigned slot;
    uint16_t x;
    uint16_t y;
    bool down;
};
using Frame = std::vector<Contact>;

struct Captured
{
    std::mutex mutex;
    std::vector<Frame> frames;
    size_t other_kinds = 0;

    void clear()
    {
        std::lock_guard<std::mutex> lock(mutex);
        frames.clear();
        other_kinds = 0;
    }
    std::vector<Frame> snapshot()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return frames;
    }
};

const Contact* inSlot(const Frame& f, unsigned slot)
{
    for (const auto& c : f)
    {
        if (c.slot == slot)
        {
            return &c;
        }
    }
    return nullptr;
}

// Widget pixels to the topic's 0..10000, for an 800x600 widget.
uint16_t nx(int px) { return static_cast<uint16_t>(px / 800.0 * 10000.0); }

}  // namespace

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    spdlog::set_level(spdlog::level::warn);

    const std::string prefix = "test/carplay_multitouch_" + std::to_string(::getpid());
    CarplayConfig_t cfg;
    cfg.video_key = prefix + "/video";
    cfg.audio_key = prefix + "/audio";
    cfg.mic_key = prefix + "/mic";
    cfg.input_key = prefix + "/input";
    cfg.session_key = prefix + "/session";
    cfg.visibility_key = prefix + "/visibility";
    cfg.return_button.enabled = true;
    cfg.return_button.label = "Vehicle";
    cfg.return_button.command.target = prefix + "_pages";
    cfg.return_button.command.action = page_action_t::go_to;
    cfg.return_button.command.page = "vehicle";

    Captured captured;
    pub_sub::ZenohTypedSubscriber<CarPlayInput> sub(
        cfg.input_key,
        [&captured](CarPlayInput::Reader reader) {
            std::lock_guard<std::mutex> lock(captured.mutex);
            if (reader.getKind() != CarPlayInput::Kind::TOUCH)
            {
                ++captured.other_kinds;
                return;
            }
            Frame f;
            for (auto c : reader.getContacts())
            {
                f.push_back({c.getSlot(), c.getX(), c.getY(), c.getDown()});
            }
            captured.frames.push_back(std::move(f));
        });

    QWidget page;
    page.resize(800, 600);
    auto* widget = new CarPlayWidget(cfg, &page);
    widget->setGeometry(0, 0, 800, 600);
    page.show();
    pump(300ms);  // let zenoh match the pub/sub pair

    QPointingDevice* touchscreen = QTest::createTouchDevice();

    // --- A tap on the return button is the button's ------------------------
    //
    // With no session the button shows. A finger on it must click it and put
    // nothing on the input topic: Qt offers the touch to this widget, which
    // declines it, and Qt then makes mouse events for the button out of it.
    {
        QPushButton* button = widget->returnButton();
        check(button != nullptr && button->isVisible(), "the return button shows with no session");
        int clicks = 0;
        if (button != nullptr)
        {
            QObject::connect(button, &QPushButton::clicked, [&clicks] { ++clicks; });
            const QPoint centre = button->geometry().center();
            captured.clear();
            QTest::touchEvent(widget, touchscreen).press(0, centre);
            QTest::touchEvent(widget, touchscreen).release(0, centre);
            pump(200ms);
        }
        check(clicks == 1, "a touch on the return button clicks it once (got " +
                               std::to_string(clicks) + ")");
        check(captured.snapshot().empty(), "and sends CarPlay nothing");
    }

    // --- A pinch ------------------------------------------------------------
    captured.clear();
    QTest::touchEvent(widget, touchscreen).press(0, QPoint(200, 200));
    pump(50ms);
    QTest::touchEvent(widget, touchscreen).stationary(0).press(1, QPoint(400, 400));
    pump(50ms);
    for (int i = 1; i <= 10; ++i)
    {
        // Apart, both at once, in one Qt event per step.
        QTest::touchEvent(widget, touchscreen)
            .move(0, QPoint(200 - (i * 10), 200 - (i * 10)))
            .move(1, QPoint(400 + (i * 10), 400 + (i * 10)));
        pump(20ms);
    }
    QTest::touchEvent(widget, touchscreen).stationary(0).release(1, QPoint(500, 500));
    pump(50ms);
    QTest::touchEvent(widget, touchscreen).release(0, QPoint(100, 100));
    pump(200ms);

    {
        const auto frames = captured.snapshot();
        check(frames.size() >= 5, "a pinch publishes frames (got " + std::to_string(frames.size()) + ")");
        check(captured.other_kinds == 0, "and nothing but frames");
        if (frames.size() >= 5)
        {
            const Frame& first = frames.front();
            check(first.size() == 1 && first[0].slot == 0 && first[0].down && first[0].x == nx(200),
                  "the first finger lands alone, in slot 0, where it touched");

            // The very next frame is the second finger landing: a mouse event
            // synthesized from the first finger would have got in between.
            const Frame& second = frames[1];
            const Contact* s0 = inSlot(second, 0);
            const Contact* s1 = inSlot(second, 1);
            check(second.size() == 2 && s0 && s1 && s0->down && s1->down && s1->x == nx(400) &&
                      s0->x == nx(200),
                  "the second finger lands in slot 1, with the first still where it was");

            bool spread = false;
            for (const auto& f : frames)
            {
                const Contact* a = inSlot(f, 0);
                const Contact* b = inSlot(f, 1);
                if (a && b && a->down && b->down && a->x < nx(200) && b->x > nx(400))
                {
                    spread = true;
                }
            }
            check(spread, "both fingers' motion arrives together, in the same frame");

            bool second_lift = false;
            for (const auto& f : frames)
            {
                const Contact* a = inSlot(f, 0);
                const Contact* b = inSlot(f, 1);
                if (a && b && a->down && !b->down && b->x == nx(500))
                {
                    second_lift = true;
                }
            }
            check(second_lift, "one finger lifting is reported with the other still down");

            const Frame& last = frames.back();
            check(last.size() == 1 && last[0].slot == 0 && !last[0].down && last[0].x == nx(100),
                  "the gesture ends with the first finger lifting, alone, where it lifted");

            bool overfull = false;
            for (const auto& f : frames)
            {
                overfull |= f.size() > 2;
            }
            check(!overfull, "no frame carries more than two fingers");
        }
    }

    // --- A third finger is not reported --------------------------------------
    captured.clear();
    QTest::touchEvent(widget, touchscreen).press(0, QPoint(100, 100));
    QTest::touchEvent(widget, touchscreen).stationary(0).press(1, QPoint(200, 200));
    QTest::touchEvent(widget, touchscreen).stationary(0).stationary(1).press(2, QPoint(300, 300));
    QTest::touchEvent(widget, touchscreen).stationary(0).stationary(1).move(2, QPoint(310, 310));
    pump(50ms);
    QTest::touchEvent(widget, touchscreen).release(0, QPoint(100, 100)).release(1, QPoint(200, 200))
        .release(2, QPoint(310, 310));
    pump(200ms);
    {
        const auto frames = captured.snapshot();
        bool bad = false;
        for (const auto& f : frames)
        {
            for (const auto& c : f)
            {
                bad |= c.slot > 1 || c.x == nx(300) || c.x == nx(310);
            }
        }
        check(!frames.empty() && !bad, "a third finger never reaches the topic");
        check(!frames.empty() && frames.back().size() == 2 && !frames.back()[0].down &&
                  !frames.back()[1].down,
              "and lifting all three lifts the two that were reported, together");
    }

    // --- Leaving the page mid-touch lifts every finger -----------------------
    captured.clear();
    QTest::touchEvent(widget, touchscreen).press(0, QPoint(100, 100));
    QTest::touchEvent(widget, touchscreen).stationary(0).press(1, QPoint(200, 200));
    pump(50ms);
    page.hide();
    pump(300ms);
    {
        const auto frames = captured.snapshot();
        check(!frames.empty() && frames.back().size() == 2 && !frames.back()[0].down &&
                  !frames.back()[1].down,
              "hiding the widget with two fingers down lifts both, in one frame");
    }

    if (g_failures == 0)
    {
        SPDLOG_WARN("all multitouch tests passed");
    }
    return g_failures == 0 ? 0 : 1;
}
