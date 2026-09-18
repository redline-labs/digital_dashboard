// SPDX-License-Identifier: GPL-3.0-or-later
//
// The one GUI-thread timer every ExpressionSubscription delivers through.
//
// Each subscription used to run its own 16 ms QTimer forever: a dozen
// bindings meant ~700 timer events a second on an idle bus, and because the
// timers were not in phase, one publisher burst was delivered -- and repainted
// -- across several event-loop passes. Here the zenoh thread arms one
// single-shot frame timer when data arrives, and that tick drains every
// subscription in one pass. With nothing arriving no timer runs at all.
//
// Staleness is the other reason something has to run with no data: a binding
// that went quiet has to be noticed. Rather than poll for it, the ticker keeps
// one timer set for the earliest moment any binding COULD go stale, so a quiet
// bus costs one wakeup per timeout, not one per frame.
//
// Header-only because the widgets build as static libraries that do not link
// dashboard_widgets; an inline function's static is still one per process.
#ifndef DASHBOARD_DELIVERY_TICKER_H_
#define DASHBOARD_DELIVERY_TICKER_H_

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <optional>
#include <vector>

#include <QCoreApplication>
#include <QObject>
#include <QPointer>
#include <QTimer>

namespace dashboard
{

// The frame the ticker coalesces into. A gauge cannot show more than the
// display refresh, so ~60 Hz is the most that can possibly be useful.
inline constexpr std::chrono::milliseconds kDeliveryInterval{16};

// What the ticker drives. Implemented by ExpressionSubscription<T>.
class DeliveryTarget
{
  public:
    using clock = std::chrono::steady_clock;

    virtual ~DeliveryTarget() = default;

    // GUI thread. Hand over whatever arrived and report staleness edges as of
    // `now`. Must be cheap when nothing arrived: it runs for every target on
    // every tick.
    virtual void drain(clock::time_point now) = 0;

    // When this target goes stale if nothing else arrives; nullopt if it
    // cannot (disabled, or already stale).
    virtual std::optional<clock::time_point> staleDeadline() const = 0;
};

class DeliveryTicker : public QObject
{
  public:
    using clock = DeliveryTarget::clock;

    explicit DeliveryTicker(QObject* parent = nullptr, std::chrono::milliseconds interval = kDeliveryInterval)
        : QObject(parent)
    {
        frame_.setSingleShot(true);
        frame_.setInterval(interval);
        QObject::connect(&frame_, &QTimer::timeout, this, [this]() { tick(); });

        // Precise: a coarse timer may fire up to 5% early, which here is just
        // a second wakeup to find nothing stale yet.
        stale_.setSingleShot(true);
        stale_.setTimerType(Qt::PreciseTimer);
        QObject::connect(&stale_, &QTimer::timeout, this, [this]() { tick(); });
    }

    // The process-wide ticker, parented to the application so it is torn down
    // with it. GUI thread only; created on first use.
    static DeliveryTicker* instance()
    {
        static QPointer<DeliveryTicker> ticker;
        if (!ticker)
        {
            ticker = new DeliveryTicker(QCoreApplication::instance());
        }
        return ticker.data();
    }

    // GUI thread.
    void add(DeliveryTarget* target)
    {
        targets_.push_back(target);
        armStaleTimer(clock::now());
    }

    // GUI thread. Safe to call from inside a delivery.
    void remove(DeliveryTarget* target)
    {
        const auto it = std::find(targets_.begin(), targets_.end(), target);
        if (it != targets_.end())
        {
            *it = nullptr;  // compacted after the pass, so an index in flight stays valid
            dirty_ = true;
        }
        if (!in_tick_)
        {
            compact();
        }
    }

    // ANY thread: data is waiting in some target. Posts at most one event per
    // frame no matter how many samples or subscriptions call it.
    void wake()
    {
        if (!armed_.exchange(true, std::memory_order_acq_rel))
        {
            QMetaObject::invokeMethod(this, [this]() {
                if (!frame_.isActive())
                {
                    frame_.start();
                }
            }, Qt::QueuedConnection);
        }
    }

    // For tests: whether anything is scheduled.
    bool frameActive() const { return frame_.isActive(); }
    bool staleTimerActive() const { return stale_.isActive(); }
    std::size_t targetCount() const
    {
        return static_cast<std::size_t>(std::count_if(targets_.begin(), targets_.end(),
                                                       [](const DeliveryTarget* t) { return t != nullptr; }));
    }

  private:
    void tick()
    {
        // Disarm BEFORE draining. A sample written after this store posts a
        // new wake; one written before it is in a mailbox this pass reads. The
        // other order loses a sample that lands between the drain and the store.
        armed_.store(false, std::memory_order_seq_cst);

        in_tick_ = true;
        const auto now = clock::now();
        for (std::size_t i = 0; i < targets_.size(); ++i)
        {
            if (DeliveryTarget* target = targets_[i])
            {
                target->drain(now);
            }
        }
        in_tick_ = false;
        compact();
        armStaleTimer(now);
    }

    void armStaleTimer(clock::time_point now)
    {
        std::optional<clock::time_point> earliest;
        for (const DeliveryTarget* target : targets_)
        {
            if (target == nullptr)
            {
                continue;
            }
            if (const auto deadline = target->staleDeadline(); deadline && (!earliest || *deadline < *earliest))
            {
                earliest = deadline;
            }
        }

        if (!earliest)
        {
            stale_.stop();
            return;
        }

        // Rounded up, so the tick lands at or after the deadline rather than
        // a millisecond short of it and has to go round again. Capped so a
        // huge timeout cannot overflow QTimer's int milliseconds; waking early
        // just sets it again.
        const auto remaining = std::clamp<clock::duration>(*earliest - now, clock::duration::zero(), std::chrono::hours{1});
        stale_.start(std::chrono::ceil<std::chrono::milliseconds>(remaining));
    }

    void compact()
    {
        if (dirty_)
        {
            targets_.erase(std::remove(targets_.begin(), targets_.end(), nullptr), targets_.end());
            dirty_ = false;
        }
    }

    std::vector<DeliveryTarget*> targets_;
    QTimer frame_;
    QTimer stale_;
    std::atomic<bool> armed_{false};
    bool in_tick_ = false;
    bool dirty_ = false;
};

}  // namespace dashboard

#endif  // DASHBOARD_DELIVERY_TICKER_H_
