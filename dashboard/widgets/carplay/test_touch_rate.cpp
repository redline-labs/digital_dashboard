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

// Everything the widget published, in order.
struct Captured
{
    std::mutex mutex;
    std::vector<CarPlayInput::Kind> kinds;
    std::vector<std::pair<uint16_t, uint16_t>> positions;

    void clear()
    {
        std::lock_guard<std::mutex> lock(mutex);
        kinds.clear();
        positions.clear();
    }

    size_t countOf(CarPlayInput::Kind kind)
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
            captured.kinds.push_back(reader.getKind());
            captured.positions.emplace_back(reader.getX(), reader.getY());
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

    const size_t moves = captured.countOf(CarPlayInput::Kind::TOUCH_MOVE);
    SPDLOG_INFO("published {} TOUCH_MOVE over ~1.2 s", moves);
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
            if (captured.kinds[i] == CarPlayInput::Kind::TOUCH_MOVE &&
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

    const size_t downs = captured.countOf(CarPlayInput::Kind::TOUCH_DOWN);
    const size_t ups = captured.countOf(CarPlayInput::Kind::TOUCH_UP);
    expect(downs == 10, "10 rapid taps publish 10 TOUCH_DOWN (got " + std::to_string(downs) + ")");
    expect(ups == 10, "10 rapid taps publish 10 TOUCH_UP (got " + std::to_string(ups) + ")");

    // --- Motion outside a drag publishes nothing ----------------------------
    captured.clear();
    for (int i = 0; i < 50; ++i)
    {
        sendMouse(&widget, QEvent::MouseMove, Qt::NoButton, QPointF(200 + i, 200));
    }
    pump(std::chrono::milliseconds(100));
    expect(captured.countOf(CarPlayInput::Kind::TOUCH_MOVE) == 0,
           "moves with no button held publish nothing");

    if (failures == 0)
    {
        SPDLOG_INFO("all touch rate tests passed");
    }
    return failures == 0 ? 0 : 1;
}
