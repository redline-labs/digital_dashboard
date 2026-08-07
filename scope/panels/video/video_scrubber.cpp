#include "video/video_scrubber.h"

#include <QMouseEvent>
#include <QPainter>

#include <algorithm>
#include <cmath>

namespace scope
{

namespace
{

// Short. This sits inside a panel whose whole point is the picture above it, so
// it takes as little of that as it can while staying grabbable -- OverviewStrip
// is 48 px because it also draws a histogram and labels, and this draws neither.
constexpr int kHeight = 18;

// The same palette as OverviewStrip, so a panel that grew its own seek bar still
// reads as part of one window.
constexpr const char* kBackground = "#14161A";
constexpr const char* kFrame = "#3A4048";
constexpr const char* kTrack = "#1B2A33";
constexpr const char* kSeekPoint = "#2E5A6E";
constexpr const char* kPlayhead = "#4FC3F7";
constexpr const char* kCursorColor = "#E0E4EA";
constexpr const char* kDisabled = "#5A616B";

}  // namespace

VideoScrubber::VideoScrubber(QWidget* parent) : QWidget(parent)
{
    setObjectName("video_scrubber");
    setMouseTracking(true);
    setFixedHeight(kHeight);
    setMinimumWidth(80);
    QWidget::setCursor(Qt::PointingHandCursor);
}

// --------------------------------------------------------------------- inputs

void VideoScrubber::setExtent(double t0, double t1)
{
    if (t1 <= t0)
    {
        // A degenerate extent would divide by zero in timeAt()/xFor() and put
        // every mark in the same column.
        t1 = t0 + 1.0;
    }
    if (extent_begin_ == t0 && extent_end_ == t1)
    {
        return;
    }
    extent_begin_ = t0;
    extent_end_ = t1;
    update();
}

void VideoScrubber::setPlayhead(double t)
{
    if (playhead_ == t)
    {
        return;
    }
    playhead_ = t;
    update();
}

void VideoScrubber::setTimeCursor(std::optional<double> t)
{
    if (cursor_ == t)
    {
        return;
    }
    cursor_ = t;
    update();
}

void VideoScrubber::setSeekPoints(std::vector<double> times)
{
    seek_points_ = std::move(times);
    update();
}

void VideoScrubber::setPlaying(bool playing)
{
    if (playing_ == playing)
    {
        return;
    }
    playing_ = playing;
    update();
}

void VideoScrubber::setSeekable(bool seekable)
{
    if (seekable_ == seekable)
    {
        return;
    }
    seekable_ = seekable;
    update();
}

// ---------------------------------------------------------------------- axis

QRectF VideoScrubber::trackRect() const
{
    return QRectF(2.0, 3.0, std::max(1.0, static_cast<double>(width()) - 4.0),
                  static_cast<double>(height()) - 6.0);
}

double VideoScrubber::timeAt(int x) const
{
    const QRectF track = trackRect();
    const double fraction = (static_cast<double>(x) - track.left()) / track.width();
    return extent_begin_ + std::clamp(fraction, 0.0, 1.0) * (extent_end_ - extent_begin_);
}

double VideoScrubber::xFor(double t) const
{
    const QRectF track = trackRect();
    const double fraction = (t - extent_begin_) / (extent_end_ - extent_begin_);
    return track.left() + std::clamp(fraction, 0.0, 1.0) * track.width();
}

// -------------------------------------------------------------------- paint

void VideoScrubber::paintEvent(QPaintEvent* /*event*/)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, false);

    painter.fillRect(rect(), QColor(kBackground));

    const QRectF track = trackRect();
    painter.fillRect(track, QColor(seekable_ ? kTrack : kBackground));
    painter.setPen(QColor(seekable_ ? kFrame : kDisabled));
    painter.drawRect(track);

    if (!seekable_)
    {
        // Deliberately not a track that looks draggable. A seek bar that does
        // nothing when dragged reads as a broken panel; an obviously inert one
        // reads as a live source, which is what it is.
        return;
    }

    // Seek points. These are the instants a scrub lands on exactly -- everything
    // between two of them is decoded forward from the left one -- so showing
    // them is showing the real granularity of the control.
    painter.setPen(QColor(kSeekPoint));
    for (const double t : seek_points_)
    {
        if (t < extent_begin_ || t > extent_end_)
        {
            continue;
        }
        const double x = xFor(t);
        painter.drawLine(QPointF(x, track.top() + 1.0), QPointF(x, track.bottom() - 1.0));
    }

    if (cursor_)
    {
        painter.setPen(QColor(kCursorColor));
        const double x = xFor(*cursor_);
        painter.drawLine(QPointF(x, track.top()), QPointF(x, track.bottom()));
    }

    // Last, so it is never hidden behind a seek point tick.
    const double x = xFor(playhead_);
    painter.setPen(QPen(QColor(kPlayhead), 2.0));
    painter.drawLine(QPointF(x, track.top()), QPointF(x, track.bottom()));
}

// -------------------------------------------------------------------- input

void VideoScrubber::mousePressEvent(QMouseEvent* event)
{
    if (!seekable_ || event->button() != Qt::LeftButton)
    {
        QWidget::mousePressEvent(event);
        return;
    }

    dragging_ = true;

    // Announced BEFORE the first seek, so the whole gesture including its first
    // instant is inside the coalescing window. Emitting it after would let the
    // press through as an immediate seek and only then start batching.
    emit interactionChanged(true);
    emit seekRequested(timeAt(event->position().toPoint().x()));
}

void VideoScrubber::mouseMoveEvent(QMouseEvent* event)
{
    const double t = timeAt(event->position().toPoint().x());

    if (dragging_)
    {
        emit seekRequested(t);
        return;
    }

    if (seekable_)
    {
        emit cursorRequested(t);
    }
}

void VideoScrubber::mouseReleaseEvent(QMouseEvent* event)
{
    if (!dragging_ || event->button() != Qt::LeftButton)
    {
        QWidget::mouseReleaseEvent(event);
        return;
    }

    dragging_ = false;

    // The final position first, then the release. The other order would let the
    // coalescing window close on the second-to-last mouse move, leaving the
    // playhead an event behind where the user let go.
    emit seekRequested(timeAt(event->position().toPoint().x()));
    emit interactionChanged(false);
}

void VideoScrubber::leaveEvent(QEvent* event)
{
    if (!dragging_)
    {
        emit cursorRequested(std::nullopt);
    }
    QWidget::leaveEvent(event);
}

}  // namespace scope
