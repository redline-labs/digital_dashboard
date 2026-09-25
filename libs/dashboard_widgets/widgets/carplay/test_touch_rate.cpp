// SPDX-License-Identifier: GPL-3.0-or-later
// Touch rate limiting in CarPlayWidget: the publish rate under a fast drag, and
// the invariants that are easy to break when adding a throttle -- the trailing
// flush after motion stops, and down/up never being limited.
//
// Drives synthetic mouse events into the widget and counts what lands on the
// zenoh input topic. Runs headless (offscreen platform), no hardware needed.
#include "carplay/carplay_widget.h"

#include "pub_sub/zenoh_subscriber.h"
#include "carplay_input.capnp.h"

#include <spdlog/spdlog.h>

#include <QtWidgets/QApplication>
#include <QtGui/QMouseEvent>
#include <QtCore/QElapsedTimer>
#include <QtCore/QCoreApplication>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

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
    else
    {
        SPDLOG_INFO("ok: {}", what);
    }
}

// What each published message amounts to for the one finger the mouse is.
enum class Touch
{
    Down,
    Move,
    Up,
    Other,
};

// Everything the widget published, in order.
struct Captured
{
    std::mutex mutex;
    std::vector<Touch> kinds;
    std::vector<std::pair<uint16_t, uint16_t>> positions;

    // The widget publishes whole touch frames. The mouse is always the one
    // finger in slot 0, so each frame is filed under what it amounts to -- a
    // landing, motion, or a lift -- which is what the rate rules below are
    // stated in.
    bool finger_down = false;

    void record(CarPlayInput::Reader reader)
    {
        if (reader.getKind() != CarPlayInput::Kind::TOUCH || reader.getContacts().size() != 1)
        {
            kinds.push_back(Touch::Other);
            positions.emplace_back(0, 0);
            return;
        }
        const auto c = reader.getContacts()[0];
        kinds.push_back(!c.getDown() ? Touch::Up : finger_down ? Touch::Move : Touch::Down);
        positions.emplace_back(c.getX(), c.getY());
        finger_down = c.getDown();
    }

    void clear()
    {
        std::lock_guard<std::mutex> lock(mutex);
        kinds.clear();
        positions.clear();
    }

    size_t countOf(Touch kind)
    {
        std::lock_guard<std::mutex> lock(mutex);
        size_t n = 0;
        for (auto k : kinds)
        {
            if (k == kind) ++n;
        }
        return n;
    }
};

// Pumps the Qt event loop for the given wall time so queued publishes and the
// flush timer actually run.
void pump(std::chrono::milliseconds duration)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < static_cast<qint64>(duration.count()))
    {
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void sendMouse(QWidget* w, QEvent::Type type, Qt::MouseButton button, QPointF pos)
{
    QMouseEvent event(type, pos, w->mapToGlobal(pos), button,
                      (type == QEvent::MouseButtonRelease) ? Qt::NoButton : Qt::LeftButton,
                      Qt::NoModifier);
    QCoreApplication::sendEvent(w, &event);
}

}  // namespace

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    Captured captured;
    CarplayConfig_t cfg;
    cfg.input_key = "test/carplay/input";

    pub_sub::ZenohTypedSubscriber<CarPlayInput> sub(
        cfg.input_key,
        [&captured](CarPlayInput::Reader reader) {
            std::lock_guard<std::mutex> lock(captured.mutex);
            captured.record(reader);
        });

    CarPlayWidget widget(cfg);
    widget.resize(800, 600);
    widget.show();
    pump(std::chrono::milliseconds(300));  // let zenoh match the pub/sub pair

    // --- A fast drag is limited to roughly 60 Hz -----------------------------
    //
    // 500 moves over 1 s is 500 Hz in, which is what a high polling rate mouse
    // produces. Out should be ~60.
    captured.clear();
    sendMouse(&widget, QEvent::MouseButtonPress, Qt::LeftButton, QPointF(100, 100));
    {
        QElapsedTimer timer;
        timer.start();
        int sent = 0;
        while (timer.elapsed() < 1000)
        {
            const double t = static_cast<double>(timer.elapsed());
            sendMouse(&widget, QEvent::MouseMove, Qt::LeftButton,
                      QPointF(100.0 + t * 0.3, 100.0 + t * 0.2));
            ++sent;
            QCoreApplication::processEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        SPDLOG_INFO("drove {} synthetic moves over ~1 s", sent);
    }
    pump(std::chrono::milliseconds(200));

    const size_t moves = captured.countOf(Touch::Move);
    SPDLOG_INFO("published {} moves over ~1.2 s", moves);
    // Generous bounds: this is a wall-clock test on a loaded machine. The point
    // is that it is near 60 and nowhere near the 500 driven in.
    expect(moves >= 35 && moves <= 90,
           "fast drag publishes ~60 moves/s, not the ~500 driven in (got " +
               std::to_string(moves) + ")");

    sendMouse(&widget, QEvent::MouseButtonRelease, Qt::NoButton, QPointF(400, 300));
    pump(std::chrono::milliseconds(100));

    // --- A drag that stops moving still reports where it came to rest --------
    //
    // The regression this guards: without a trailing flush, the last move of a
    // gesture is swallowed by the rate limit and the phone's idea of the finger
    // stays one interval behind, forever, because no further events arrive.
    captured.clear();
    sendMouse(&widget, QEvent::MouseButtonPress, Qt::LeftButton, QPointF(10, 10));
    // Two moves back to back: the first passes the leading edge, the second is
    // inside the interval and can only reach the topic via the flush.
    sendMouse(&widget, QEvent::MouseMove, Qt::LeftButton, QPointF(11, 11));
    sendMouse(&widget, QEvent::MouseMove, Qt::LeftButton, QPointF(700, 500));
    pump(std::chrono::milliseconds(200));  // well past one 16 ms interval

    {
        std::lock_guard<std::mutex> lock(captured.mutex);
        const auto expected_x = static_cast<uint16_t>((700.0 / 800.0) * 10000.0);
        const auto expected_y = static_cast<uint16_t>((500.0 / 600.0) * 10000.0);
        bool found_rest = false;
        for (size_t i = 0; i < captured.kinds.size(); ++i)
        {
            if (captured.kinds[i] == Touch::Move &&
                captured.positions[i].first == expected_x &&
                captured.positions[i].second == expected_y)
            {
                found_rest = true;
            }
        }
        expect(found_rest, "the final position of a drag that stops is flushed, not swallowed");
    }
    sendMouse(&widget, QEvent::MouseButtonRelease, Qt::NoButton, QPointF(700, 500));
    pump(std::chrono::milliseconds(100));

    // --- Down and up are never rate limited ---------------------------------
    //
    // They are state transitions, not samples. Ten taps back to back, faster
    // than the move interval, must produce ten of each.
    captured.clear();
    for (int i = 0; i < 10; ++i)
    {
        sendMouse(&widget, QEvent::MouseButtonPress, Qt::LeftButton, QPointF(50 + i, 50));
        sendMouse(&widget, QEvent::MouseButtonRelease, Qt::NoButton, QPointF(50 + i, 50));
        QCoreApplication::processEvents();
    }
    pump(std::chrono::milliseconds(200));

    const size_t downs = captured.countOf(Touch::Down);
    const size_t ups = captured.countOf(Touch::Up);
    expect(downs == 10, "10 rapid taps publish 10 landings (got " + std::to_string(downs) + ")");
    expect(ups == 10, "10 rapid taps publish 10 lifts (got " + std::to_string(ups) + ")");

    // --- Motion outside a drag publishes nothing ----------------------------
    captured.clear();
    for (int i = 0; i < 50; ++i)
    {
        sendMouse(&widget, QEvent::MouseMove, Qt::NoButton, QPointF(200 + i, 200));
    }
    pump(std::chrono::milliseconds(100));
    expect(captured.countOf(Touch::Move) == 0,
           "moves with no button held publish nothing");

    if (failures == 0)
    {
        SPDLOG_INFO("all touch rate tests passed");
    }
    return failures == 0 ? 0 : 1;
}
