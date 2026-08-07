#ifndef SCOPE_OVERVIEW_STRIP_H_
#define SCOPE_OVERVIEW_STRIP_H_

#include "scope/time_axis.h"

#include <QPoint>
#include <QWidget>

#include <cstdint>
#include <optional>
#include <vector>

namespace scope
{

// The whole recording at a glance, with the view window drawn on it.
//
// This replaced the transport QSlider, and the reason is worth stating: a
// slider positions one value in a range whose CONTENTS you cannot see, so
// finding an event in half an hour of capture meant dragging blind and watching
// the plot. Here the background is where the messages actually are, the view is
// a region you can grab, and the cursor and the playhead are on the same axis as
// the panels'.
//
// IT IS A DUMB PAINTER. It knows nothing about DataSource, RecordedProvider or
// CaptureBuffer -- ScopeWindow pushes numbers in and connects to what comes out.
// That matches how the rest of scope is layered (no panel ever learns which kind
// of source is behind it) and means it can be tested with four setters and a
// synthesised drag.
class OverviewStrip : public QWidget
{
    Q_OBJECT

  public:
    explicit OverviewStrip(QWidget* parent = nullptr);

    // Everything below is on the SOURCE's clock. ScopeWindow does every
    // conversion, because it is the only thing that knows both clocks.

    // The full extent the strip represents: the whole recording, or as far back
    // as a live source still retains.
    void setExtent(double t0, double t1);

    // The background histogram, carrying the extent it was computed FOR.
    //
    // Stored separately from the current extent on purpose: the extent is
    // re-read every frame and a growing capture moves it constantly, while the
    // histogram is recomputed on a throttle. Drawing the cached counts against
    // the range they were counted over keeps a stale histogram in the right
    // PLACE rather than smeared across a range it never described.
    void setDensity(std::vector<std::uint32_t> counts, double t0, double t1);

    void setView(double begin, double end);

    // NOT setCursor(). QWidget::setCursor(QCursor) is an overload set this would
    // join, and Qt::CursorShape converts to int converts to double converts to
    // optional<double> -- so setCursor(Qt::OpenHandCursor) silently binds to the
    // time-cursor overload and parks the shared cursor at "13 seconds". It
    // compiles, it runs, and the only symptom is a stray line on every panel.
    void setTimeCursor(std::optional<double> t);

    // The playhead, and whether there is one at all: a live source has no
    // position to seek to, so it draws none.
    void setPlayhead(std::optional<double> t);

    // The stretch the panels' buffers can actually draw. On a live source this
    // is NARROWER than the extent, and the gap is the whole visual argument for
    // pressing Review -- it is the difference between what was captured and
    // what is on screen.
    void setRetained(double t0, double t1);

    // Non-zero once the capture has started dropping its head. Marks where the
    // reviewable session begins, because a trace starting partway through
    // otherwise reads as a publisher that had not started yet.
    void setEvicted(std::uint64_t count);

  signals:
    // A drag or a click asked for a different window. ScopeWindow decides what
    // that means -- the strip never touches the time base itself, so there is
    // one place where a view change turns into a seek.
    void viewRequested(double begin, double end);

    // Pressed or released, so the window can coalesce the seeks a drag makes.
    void interactionChanged(bool active);

    // Hovering the strip moves the same shared cursor the panels set.
    void cursorRequested(std::optional<double> t);

  protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

  private:
    // What a press landed on. The edges are hit-tested FIRST and with a margin
    // wider than the line they draw, because a two-pixel target is unusable and
    // grabbing the body when you meant the edge silently pans instead of zooms.
    enum class Grab
    {
        None,
        Body,
        LeftEdge,
        RightEdge,
    };

    TimeAxis axis() const;
    Grab hitTest(const QPoint& pos) const;
    QRectF trackRect() const;

    double extent_begin_ = 0.0;
    double extent_end_ = 1.0;

    std::vector<std::uint32_t> density_;
    double density_begin_ = 0.0;
    double density_end_ = 1.0;

    double view_begin_ = 0.0;
    double view_end_ = 1.0;

    std::optional<double> cursor_;
    std::optional<double> playhead_;

    double retained_begin_ = 0.0;
    double retained_end_ = 1.0;

    std::uint64_t evicted_ = 0;

    Grab grab_ = Grab::None;

    // Where in the view region the press landed, as an offset in seconds. Held
    // so a body drag moves the window WITH the pointer rather than centring it
    // on the pointer, which would make the region jump on the first pixel.
    double grab_offset_ = 0.0;
};

}  // namespace scope

#endif  // SCOPE_OVERVIEW_STRIP_H_
