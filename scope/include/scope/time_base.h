#ifndef SCOPE_TIME_BASE_H_
#define SCOPE_TIME_BASE_H_

#include "scope/data_source.h"

#include <QObject>
#include <QTimer>

#include <optional>
#include <utility>

namespace scope
{

// The window's shared clock, and the one timer that drives every panel.
//
// Panels do not own timers. One timer for the window keeps them frame-synced --
// so a value read off two panels under the shared cursor is from the same
// instant -- and avoids the per-widget timer sprawl the dashboard's sparkline
// has, where every instance schedules its own repaint at its own configured
// rate and they drift against each other.
//
// The transport bar renders from DataSource::caps(): Follow/Pause for a live
// source, or playback controls for a seekable one. Panels ask the time base
// what the view window is; none of them learns which kind of source is behind
// it. The overview strip is fed the same numbers and is likewise unaware.
//
// THE VIEW WINDOW IS EXPLICIT. It used to be derived -- a right edge pinned to
// the source's clock, minus a span -- which is exactly the model that cannot be
// zoomed or panned, because both of those move the left and right edges
// independently. `view_begin_`/`view_end_` are now stored, and `follow_` says
// whether something other than the user is driving the right edge: the source's
// clock on a live source, playback on a recorded one. Every pan and zoom clears
// it, because you cannot hold a span still while it is pinned to now.
class TimeBase : public QObject
{
    Q_OBJECT

  public:
    // Kept as a wrapper over follow_ rather than deleted, because it is the
    // vocabulary the agent interface and the transport bar already use:
    // `scope.time_base` takes mode: "live" | "paused", and a mode enum reads
    // better at those call sites than a bare bool. There is only one flag
    // underneath.
    enum class Mode
    {
        // Right edge tracks the source's clock; the view scrolls.
        Live,

        // The view is frozen. Data still arrives and is still buffered -- only
        // the displayed window stops moving, so unpausing does not leave a gap.
        Paused,
    };

    // `source` must outlive this object, or be replaced before it dies.
    explicit TimeBase(DataSource& source, QObject* parent = nullptr);
    ~TimeBase() override;

    // Point at a different source -- going into review over a recording, or
    // back to live.
    //
    // Resets the view and clears `cursor_`. Both are values on the OLD source's
    // epoch and mean nothing on the new one: a live source counts seconds since
    // it was constructed, a recorded one seconds since the recording started.
    // Carried over, the window would sit somewhere arbitrary in the recording
    // and the shared cursor would read out a time that does not exist --
    // silently, because both are just doubles.
    //
    // The SPAN survives, because it is a preference rather than a position:
    // someone reviewing at a 5-second window wants a 5-second window in the
    // next recording too. Only where it sits is reset.
    void setSource(DataSource& source);

    // Seconds of history the window shows: the width of [viewBegin, viewEnd].
    // Setting it zooms about the RIGHT edge, which is what a "show me the last
    // N seconds" control means and what the spin box has always done.
    double windowSeconds() const { return window_seconds_; }
    void setWindowSeconds(double seconds);

    Mode mode() const { return follow_ ? Mode::Live : Mode::Paused; }
    void setMode(Mode mode) { setFollowing(mode == Mode::Live); }

    int renderRateHz() const { return render_rate_hz_; }
    void setRenderRateHz(int hz);

    // ------------------------------------------------------------ the view
    //
    // The window every panel draws.
    //
    // STORED AS (RIGHT EDGE, SPAN), not as a pair, and the asymmetry is
    // deliberate three times over:
    //
    //   - The span is the thing that persists. `scope_workspace_t` saves it,
    //     the spin box edits it, and a panel with follow_time_base off
    //     overrides it -- so it is a field in its own right rather than a
    //     subtraction.
    //   - It gives setWindowSeconds() a defined answer to "which edge moves?".
    //     With a stored pair there is none.
    //   - A FOLLOWING RIGHT EDGE HAS TO BE DERIVED. Stored, it is only correct
    //     until the source's clock advances, so everything reading it between
    //     two render ticks -- the agent interface, the transport readout, a
    //     panel repainting for its own reasons -- gets a window that is up to a
    //     frame stale, silently.
    double viewEnd() const { return follow_ ? source_->now() : view_end_; }
    double viewBegin() const { return viewEnd() - window_seconds_; }

    // Set both edges at once. THE primitive: zoomAt(), panBy(), fitAll() and
    // setWindowSeconds() all end up here, so the clamping and the side effects
    // are written once.
    //
    // Clamps the span to [kMinWindowSeconds, kMaxWindowSeconds] and then slides
    // -- not squashes -- the window into availableRange(), narrowing it only
    // when the range itself is narrower. Turns following off, and stops playback
    // on a seekable source: a view pinned to now cannot be held still, and
    // playback would drag you off the span you just chose at the render rate.
    void setView(double begin, double end);

    // Zoom about a fixed instant: `anchor_t` keeps its position within the
    // window, so the sample under the mouse stays under the mouse. `factor`
    // multiplies the span -- below 1 zooms in.
    void zoomAt(double anchor_t, double factor);

    // Slide the window by `dt` seconds without changing its width.
    void panBy(double dt);

    // Show everything there is: the view becomes availableRange(). On a live
    // source this also means "as far back as anything is retained".
    void fitAll();

    // What setView() will clamp to.
    //
    // Seekable: the recording's extent, straight from caps(). Live: caps()
    // carries no meaningful t_begin -- a bus has no beginning -- so it is
    // [now - retention, now], the window the panels' buffers can actually
    // answer for. Zooming out past what is retained would show emptiness that
    // looks exactly like a publisher that had not started.
    std::pair<double, double> availableRange() const;

    // Seconds of samples the panels retain, so availableRange() can say how far
    // back a live view may go. Fed from ScopeWindow::historySeconds(), which is
    // the workspace-level setting the buffers are actually built with -- this is
    // a copy for the clamp, not a second source of truth.
    double retentionSeconds() const { return retention_seconds_; }
    void setRetentionSeconds(double seconds);

    // Is the right edge being driven by something other than the user? The
    // source's clock on a live source, playback on a recorded one.
    bool following() const { return follow_; }

    // Turning it on snaps the right edge to the source's clock, keeping the
    // current span. Turning it off leaves the window exactly where it is.
    void setFollowing(bool on);

    // Held for the duration of a drag, so the seeks a gesture generates are
    // coalesced to one per render tick.
    //
    // A drag emits a mouse-move per pass of the event loop -- 60 to 125 a
    // second -- and on a recorded source every view change is a seek, which
    // refills a WHOLE retention window per bound signal
    // (SignalBuffer::replaceHistory). Eight traces of a 1 kHz signal over the
    // default 300 s retention is 2.4 million samples rebuilt per event, and the
    // drag stutters in proportion to how much history is retained -- which is
    // the opposite of what retaining more should cost.
    //
    // Outside an interaction a seek is applied immediately, so a caller that
    // sets the view and then reads sample_stats -- the agent interface, a test
    // -- sees the buffers it asked for rather than the ones from before. This is
    // the same distinction QSlider::isSliderDown() draws, and the transport bar
    // already relies on it.
    bool interacting() const { return interacting_; }
    void setInteracting(bool on);

    // Where the hover cursor is, if anywhere. Shared across panels: one cursor
    // at one instant, read out by every panel that has data there.
    const std::optional<double>& cursor() const { return cursor_; }
    void setCursor(std::optional<double> t);

    DataSource& source() { return *source_; }
    const DataSource& source() const { return *source_; }

    // -------------------------------------------------------------- playback
    //
    // No-ops on a source that is not seekable, so a caller that did not check
    // caps() first gets nothing rather than an error.

    // Move the playback position, and the view's right edge with it.
    //
    // ON A RECORDED SOURCE THE PLAYHEAD AND THE VIEW'S RIGHT EDGE ARE THE SAME
    // THING, and that is a rule rather than a coincidence. RecordedSource loads
    // [t - history, t] around ONE position on every seek
    // (SignalBuffer::replaceHistory), so a view that could sit somewhere other
    // than the playhead would be drawing from buffers that hold a different
    // stretch of the recording -- and it would look like data rather than like a
    // bug. setView() therefore seeks to its right edge, and seek() moves the
    // window so its right edge is `t`, keeping the span.
    void seek(double t);

    // Playback speed, as a multiplier on real time. Zero is not allowed -- that
    // is what setPlaying(false) means -- and the clamp is wide because both ends
    // are useful: 0.1x to study a transient, 20x to find one.
    double rate() const { return rate_; }
    void setRate(double rate);

    bool playing() const { return playing_; }
    void setPlaying(bool playing);

  signals:
    // The window, the mode or the rate changed: panels should rescale.
    void changed();

    // The source was replaced. The transport bar re-reads caps() from it.
    void sourceChanged();

    // One tick of the render timer. Panels drain their buffers and repaint.
    void frame();

    // The shared cursor moved (or was cleared).
    void cursorMoved();

  private:
    void restartTimer();

    // setView() without the "this was the user" side effects: no clearing of
    // follow_, no stopping playback. What the render tick uses to carry a
    // following window forward, and what setFollowing(true) uses to snap it.
    void applyView(double begin, double end);

    // Hand the view's right edge to a seekable source, unless it is already
    // there. Called immediately outside an interaction and once per render tick
    // during one -- see setInteracting().
    void flushSeek();

    // A pointer, not a reference, because it is reseated: entering review over
    // a recording swaps the whole source out from under the window.
    DataSource* source_;
    QTimer timer_;

    int render_rate_hz_ = 30;

    double window_seconds_ = 30.0;

    // Where the right edge sits when it is NOT being followed. Meaningless
    // while follow_ is set -- viewEnd() derives it from the source then -- and
    // captured at the moment following stops, which is what freezes a paused
    // view where it was rather than where the source has got to.
    double view_end_ = 0.0;

    bool follow_ = true;

    // A drag is in progress, so seeks coalesce to the render tick.
    bool interacting_ = false;

    // The right edge a seekable source has not been told about yet.
    std::optional<double> pending_seek_;

    // How far back availableRange() lets a live view go. The panels' retention,
    // copied here for the clamp.
    double retention_seconds_ = 300.0;

    std::optional<double> cursor_;

    double rate_ = 1.0;
    bool playing_ = false;
};

}  // namespace scope

#endif  // SCOPE_TIME_BASE_H_
