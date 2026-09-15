#include "scope/overview_strip.h"

#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace scope
{

namespace
{

// Tall enough for a readable histogram and a row of labels, short enough that it
// costs the plots almost nothing.
constexpr int kHeight = 48;
constexpr double kLabelHeight = 13.0;

// A grab margin wider than the line it targets. Two pixels is unusable, and
// missing an edge grabs the body instead -- which pans when the user meant to
// zoom, silently and in the wrong dimension.
constexpr double kEdgeGrab = 6.0;

// The panel's palette, so the strip reads as part of the same window rather
// than as a different app bolted underneath it.
constexpr const char* kBackground = "#14161A";
constexpr const char* kFrame = "#3A4048";
constexpr const char* kLabel = "#8A94A6";
constexpr const char* kCursor = "#E0E4EA";
constexpr const char* kDensity = "#2E5A6E";
constexpr const char* kRetained = "#1B2A33";
constexpr const char* kViewEdge = "#4FC3F7";

QString formatOffset(double seconds)
{
    // Relative m:ss, matching the panels' relative axis labels. A recording's
    // wall clock belongs in the cursor readout, where there is room to say what
    // it is.
    const auto total = static_cast<int>(std::floor(std::max(seconds, 0.0)));
    return QStringLiteral("%1:%2")
        .arg(total / 60)
        .arg(total % 60, 2, 10, QLatin1Char('0'));
}

}  // namespace

OverviewStrip::OverviewStrip(QWidget* parent) : QWidget(parent)
{
    setObjectName("overview_strip");
    setMouseTracking(true);
    setFixedHeight(kHeight);
    setMinimumWidth(200);
    QWidget::setCursor(Qt::PointingHandCursor);
}

// ------------------------------------------------------------------- inputs

void OverviewStrip::setExtent(double t0, double t1)
{
    if (t1 <= t0)
    {
        // A source with nothing in it yet. A degenerate extent would divide by
        // zero in the axis and put every mark in the same column.
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

void OverviewStrip::setDensity(const std::vector<std::uint32_t>& counts, double t0, double t1)
{
    if (density_ == counts && density_begin_ == t0 && density_end_ == t1)
    {
        return;
    }
    density_ = counts;
    density_begin_ = t0;
    density_end_ = t1;
    update();
}

void OverviewStrip::setView(double begin, double end)
{
    if (view_begin_ == begin && view_end_ == end)
    {
        return;
    }
    view_begin_ = begin;
    view_end_ = end;
    update();
}

void OverviewStrip::setTimeCursor(std::optional<double> t)
{
    if (cursor_ == t)
    {
        return;
    }
    cursor_ = t;
    update();
}

void OverviewStrip::setPlayhead(std::optional<double> t)
{
    if (playhead_ == t)
    {
        return;
    }
    playhead_ = t;
    update();
}

void OverviewStrip::setRetained(double t0, double t1)
{
    if (retained_begin_ == t0 && retained_end_ == t1)
    {
        return;
    }
    retained_begin_ = t0;
    retained_end_ = t1;
    update();
}

void OverviewStrip::setEvicted(std::uint64_t count)
{
    if (evicted_ == count)
    {
        return;
    }
    evicted_ = count;
    update();
}

// ------------------------------------------------------------------ geometry

QRectF OverviewStrip::trackRect() const
{
    return QRectF(0.0, 0.0, static_cast<double>(width()),
                  static_cast<double>(height()) - kLabelHeight);
}

TimeAxis OverviewStrip::axis() const
{
    const QRectF track = trackRect();
    TimeAxis axis;
    axis.t0 = extent_begin_;
    axis.t1 = extent_end_;
    axis.x0 = track.left();
    axis.x1 = track.right();
    return axis;
}

OverviewStrip::Grab OverviewStrip::hitTest(const QPoint& pos) const
{
    const TimeAxis a = axis();
    if (!a.usable())
    {
        return Grab::None;
    }

    const double x = pos.x();
    const double left = a.toX(view_begin_);
    const double right = a.toX(view_end_);

    // Edges first, and with the margin. Testing the body first would make the
    // edges unreachable on any view wider than the grab margin -- which is
    // almost all of them.
    if (std::abs(x - left) <= kEdgeGrab)
    {
        return Grab::LeftEdge;
    }
    if (std::abs(x - right) <= kEdgeGrab)
    {
        return Grab::RightEdge;
    }
    if (x > left && x < right)
    {
        return Grab::Body;
    }
    return Grab::None;
}

// ------------------------------------------------------------------ painting

void OverviewStrip::paintEvent(QPaintEvent* /*event*/)
{
    QPainter painter(this);
    const QRectF track = trackRect();
    const TimeAxis a = axis();

    painter.fillRect(rect(), QColor(kBackground));
    if (!a.usable())
    {
        return;
    }

    // ------------------------------------------- what the panels can draw
    //
    // Behind the histogram, because it is a backdrop rather than data. On a
    // live source it is a slice of the middle; on a recording it is everything.
    {
        const double x0 = a.toClampedX(retained_begin_);
        const double x1 = a.toClampedX(retained_end_);
        painter.fillRect(QRectF(x0, track.top(), x1 - x0, track.height()), QColor(kRetained));
    }

    // ------------------------------------------------------------ histogram

    if (!density_.empty() && density_end_ > density_begin_)
    {
        const auto peak = *std::max_element(density_.begin(), density_.end());
        if (peak > 0)
        {
            const double bucket_span =
                (density_end_ - density_begin_) / static_cast<double>(density_.size());

            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(kDensity));

            for (std::size_t i = 0; i < density_.size(); ++i)
            {
                if (density_[i] == 0)
                {
                    continue;
                }

                // LOG SCALE. A 1 kHz CAN topic and a 1 Hz telemetry one differ
                // by three orders of magnitude, and on a linear scale the
                // quiet stretch is a flat line -- which is exactly the stretch
                // someone scrubbing is usually looking for. The same reasoning
                // as the plot's two vertical axes.
                const double fraction = std::log1p(static_cast<double>(density_[i])) /
                                        std::log1p(static_cast<double>(peak));
                const double h = fraction * track.height();

                // Drawn against the range the counts were COUNTED over, not the
                // current extent: a capture grows between recomputations, and
                // this keeps a stale histogram in the right place rather than
                // stretched over a range it never described.
                const double x0 = a.toX(density_begin_ + static_cast<double>(i) * bucket_span);
                const double x1 =
                    a.toX(density_begin_ + static_cast<double>(i + 1) * bucket_span);

                // At least a pixel: a bucket with messages in it must not
                // vanish because the widget is wider than the histogram is long.
                painter.drawRect(QRectF(x0, track.bottom() - h, std::max(x1 - x0, 1.0), h));
            }
        }
    }

    // -------------------------------------------------------- eviction head

    if (evicted_ > 0)
    {
        // The reviewable session starts HERE, not at the beginning of anything.
        // Without the mark, a trace that begins partway through reads as a
        // publisher that had not started yet.
        painter.setPen(QPen(QColor("#8A6060"), 2.0));
        painter.drawLine(QPointF(a.x0 + 1.0, track.top()), QPointF(a.x0 + 1.0, track.bottom()));
    }

    // ----------------------------------------------------------- the window

    {
        const double x0 = a.toClampedX(view_begin_);
        const double x1 = a.toClampedX(view_end_);

        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(79, 195, 247, 36));
        painter.drawRect(QRectF(x0, track.top(), std::max(x1 - x0, 2.0), track.height()));

        painter.setPen(QPen(QColor(kViewEdge), 1.5));
        painter.drawLine(QPointF(x0, track.top()), QPointF(x0, track.bottom()));
        painter.drawLine(QPointF(x1, track.top()), QPointF(x1, track.bottom()));
    }

    // --------------------------------------------------- playhead and cursor

    if (playhead_)
    {
        const double x = a.toClampedX(*playhead_);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(kCursor));

        // A downward triangle at the top, so it reads as a position marker
        // rather than as another edge of the window.
        const QPointF tip(x, track.top() + 6.0);
        const QPointF left(x - 4.0, track.top());
        const QPointF right(x + 4.0, track.top());
        painter.drawPolygon(QPolygonF({tip, left, right}));
    }

    if (cursor_)
    {
        // The SAME shared cursor the panels draw, on the same clock, so the
        // instant being read out has a place on the overview too.
        painter.setPen(QPen(QColor(kCursor), 1.0, Qt::DashLine));
        const double x = a.toClampedX(*cursor_);
        painter.drawLine(QPointF(x, track.top()), QPointF(x, track.bottom()));
    }

    // NoBrush FIRST. drawRect strokes with the pen and FILLS with the current
    // brush, and the brush at this point is whatever the last filled shape left
    // behind -- the playhead's near-white. Without this the frame paints a solid
    // block over the entire track, and only on a seekable source, because a live
    // one draws no playhead and leaves the brush a dark colour that looks
    // plausible.
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(kFrame), 1.0));
    painter.drawRect(track.adjusted(0.0, 0.0, -1.0, -1.0));

    // ---------------------------------------------------------------- labels

    painter.setPen(QColor(kLabel));
    QFont font = painter.font();
    font.setPointSizeF(8.0);
    painter.setFont(font);

    const QRectF labels(0.0, track.bottom(), static_cast<double>(width()), kLabelHeight);
    painter.drawText(labels, Qt::AlignLeft | Qt::AlignVCenter,
                     QStringLiteral(" %1").arg(formatOffset(0.0)));
    painter.drawText(labels, Qt::AlignRight | Qt::AlignVCenter,
                     QStringLiteral("%1 ").arg(formatOffset(extent_end_ - extent_begin_)));
}

// --------------------------------------------------------------- interaction

void OverviewStrip::mousePressEvent(QMouseEvent* event)
{
    const TimeAxis a = axis();
    if (event->button() != Qt::LeftButton || !a.usable())
    {
        QWidget::mousePressEvent(event);
        return;
    }

    grab_ = hitTest(event->pos());

    if (grab_ == Grab::None)
    {
        // Clicked outside the window: centre it there. Jumping to a place you
        // pointed at is the one thing the old slider did well, and losing it
        // would make the strip worse for the coarse case it is best at.
        const double span = view_end_ - view_begin_;
        const double centre = a.toT(event->position().x());
        emit viewRequested(centre - span / 2.0, centre + span / 2.0);
        QWidget::mousePressEvent(event);
        return;
    }

    grab_offset_ = a.toT(event->position().x()) - view_begin_;
    QWidget::mousePressEvent(event);
}

void OverviewStrip::mouseMoveEvent(QMouseEvent* event)
{
    const TimeAxis a = axis();
    if (!a.usable())
    {
        QWidget::mouseMoveEvent(event);
        return;
    }

    const double t = a.toT(event->position().x());

    if (grab_ == Grab::None)
    {
        // Hovering feeds the same shared cursor a panel does, so a readout
        // taken off the strip lines up with the plots.
        QWidget::setCursor(hitTest(event->pos()) == Grab::Body ? Qt::OpenHandCursor
                                                               : Qt::PointingHandCursor);
        emit cursorRequested(t);
        QWidget::mouseMoveEvent(event);
        return;
    }

    switch (grab_)
    {
        case Grab::Body:
        {
            // Moves WITH the pointer, keeping the grab offset, rather than
            // centring on it -- otherwise the region jumps on the first pixel of
            // every drag.
            const double span = view_end_ - view_begin_;
            const double begin = t - grab_offset_;
            emit viewRequested(begin, begin + span);
            break;
        }
        case Grab::LeftEdge:
            emit viewRequested(t, view_end_);
            break;
        case Grab::RightEdge:
            emit viewRequested(view_begin_, t);
            break;
        case Grab::None:
            break;
    }

    QWidget::mouseMoveEvent(event);
}

void OverviewStrip::mouseReleaseEvent(QMouseEvent* event)
{
    if (grab_ != Grab::None)
    {
        grab_ = Grab::None;
    }
    QWidget::mouseReleaseEvent(event);
}

void OverviewStrip::leaveEvent(QEvent* event)
{
    emit cursorRequested(std::nullopt);
    QWidget::leaveEvent(event);
}

void OverviewStrip::wheelEvent(QWheelEvent* event)
{
    const TimeAxis a = axis();
    const double delta = static_cast<double>(event->angleDelta().y());
    if (!a.usable() || delta == 0.0)
    {
        QWidget::wheelEvent(event);
        return;
    }

    // Same gesture and the same feel as over a plot, so the strip is not a
    // second set of rules to learn.
    const double factor = std::pow(1.0015, -delta);
    const double anchor = a.toT(event->position().x());
    const double span = view_end_ - view_begin_;
    const double at = std::clamp((anchor - view_begin_) / span, 0.0, 1.0);
    const double new_span = span * factor;

    emit viewRequested(anchor - at * new_span, anchor + (1.0 - at) * new_span);
    event->accept();
}

}  // namespace scope

#include "scope/moc_overview_strip.cpp"
