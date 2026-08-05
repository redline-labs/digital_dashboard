#ifndef SCOPE_TIME_BASE_H_
#define SCOPE_TIME_BASE_H_

#include "scope/data_source.h"

#include <QObject>
#include <QTimer>

#include <optional>

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
// This is also where recorded playback will land. The transport bar renders
// from DataSource::caps(): Pause/Live for a live source, or a scrubber and a
// rate control for a seekable one. Panels ask the time base what time it is and
// how wide the window is; none of them learns which kind of source is behind it.
class TimeBase : public QObject
{
    Q_OBJECT

  public:
    enum class Mode
    {
        // Right edge tracks the source's clock; the view scrolls.
        Live,

        // The view is frozen. Data still arrives and is still buffered -- only
        // the displayed window stops moving, so unpausing does not leave a gap.
        Paused,
    };

    // `source` must outlive this object.
    explicit TimeBase(DataSource& source, QObject* parent = nullptr);
    ~TimeBase() override;

    // Seconds of history the window shows.
    double windowSeconds() const { return window_seconds_; }
    void setWindowSeconds(double seconds);

    Mode mode() const { return mode_; }
    void setMode(Mode mode);

    int renderRateHz() const { return render_rate_hz_; }
    void setRenderRateHz(int hz);

    // The time at the right edge of the window. Advances with the source in
    // Live mode; frozen at the moment of pausing otherwise.
    double viewEnd() const;
    double viewBegin() const { return viewEnd() - window_seconds_; }

    // Where the hover cursor is, if anywhere. Shared across panels: one cursor
    // at one instant, read out by every panel that has data there.
    const std::optional<double>& cursor() const { return cursor_; }
    void setCursor(std::optional<double> t);

    DataSource& source() { return source_; }
    const DataSource& source() const { return source_; }

  signals:
    // The window, the mode or the rate changed: panels should rescale.
    void changed();

    // One tick of the render timer. Panels drain their buffers and repaint.
    void frame();

    // The shared cursor moved (or was cleared).
    void cursorMoved();

  private:
    void restartTimer();

    DataSource& source_;
    QTimer timer_;

    double window_seconds_ = 30.0;
    int render_rate_hz_ = 30;
    Mode mode_ = Mode::Live;

    // Set when entering Paused: the source keeps advancing, so the frozen right
    // edge has to be remembered rather than recomputed.
    std::optional<double> paused_at_;

    std::optional<double> cursor_;
};

}  // namespace scope

#endif  // SCOPE_TIME_BASE_H_
