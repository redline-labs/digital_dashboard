#include "segment_readout/segment_readout.h"

#include <QFontMetricsF>
#include <QPainter>

#include <spdlog/spdlog.h>

#include "dashboard/widget_colors.h"
#include "dashboard/widget_fonts.h"

#include <algorithm>
#include <cmath>

namespace
{

// DSEG renders every segment of a cell for '~' ("All-on (Exclamation)" is '!'
// for all-off, '8' or tilde for all-on -- see the bundled DSEG README). Tilde
// works for both the seven- and fourteen-segment faces, where '8' only makes
// sense on the seven.
constexpr QChar kAllSegmentsOn = QLatin1Char('~');

// Shown when a value needs more cells than the readout has, the way a real
// instrument does. Printing the number anyway would draw it wider than the
// widget and straight over whatever sits next to it.
constexpr QChar kOverflow = QLatin1Char('-');

// How many cells a string occupies on a DSEG face. The period has zero width --
// it lights the decimal point of the cell before it rather than advancing -- so
// "53.60" is four cells, not five.
int cellCount(const QString& text)
{
    int cells = 0;
    for (const QChar c : text)
    {
        if (c != QLatin1Char('.'))
        {
            ++cells;
        }
    }
    return cells;
}

// Point size is searched rather than derived: DSEG's cell width is not a fixed
// fraction of its point size across the two faces, and the readout has to fit a
// box the layout chose. Bisection over a decade of point sizes converges in a
// handful of steps and only runs when the widget is resized.
constexpr int kFitIterations = 12;
constexpr qreal kMinPointSize = 4.0;
constexpr qreal kMaxPointSize = 400.0;

constexpr float kCaptionFractionOfHeight = 0.24f;
constexpr float kCaptionGapFraction = 0.10f;

}  // namespace

SegmentReadoutWidget::SegmentReadoutWidget(const SegmentReadoutConfig_t& cfg, QWidget* parent) :
    QWidget(parent),
    _cfg{cfg}
{
    // Every case named, no default: adding a face is a build error here rather
    // than a silent fall back to the seven-segment font.
    switch (_cfg.face)
    {
        case SegmentFace::seven:
            _segment_family = dashboard::loadResourceFont(":/fonts/DSEG7Classic-Bold.ttf",
                                                          "Courier New");
            break;
        case SegmentFace::fourteen:
            _segment_family = dashboard::loadResourceFont(":/fonts/DSEG14Classic-Regular.ttf",
                                                          "Courier New");
            break;
    }

    _caption_family = dashboard::loadResourceFont(":/fonts/futura.ttf", "Helvetica");

    _ghost = QString(static_cast<int>(_cfg.digits), kAllSegmentsOn);

    _prefix = QString::fromStdString(_cfg.prefix);
    if (cellCount(_prefix) >= static_cast<int>(_cfg.digits))
    {
        // A prefix that fills the field leaves nowhere for the reading. Dropping
        // it keeps the number visible, which is the half worth keeping.
        SPDLOG_WARN("[segment_readout] prefix '{}' needs {} of {} cells, leaving no room "
                    "for the value; ignoring it",
                    _cfg.prefix, cellCount(_prefix), _cfg.digits);
        _prefix.clear();
    }

    _text = QString::fromStdString(_cfg.static_text);
    if (cellCount(_text) > static_cast<int>(_cfg.digits))
    {
        SPDLOG_WARN("[segment_readout] static_text '{}' needs {} cells but digits is {}; truncating",
                    _cfg.static_text, cellCount(_text), _cfg.digits);
        _text.truncate(static_cast<int>(_cfg.digits));
    }

    if (!_cfg.value_expression.empty())
    {
        _expression_parser = dashboard::makeExpressionSubscription<double>(
            _cfg.schema_type, _cfg.value_expression, _cfg.zenoh_key,
            this, &SegmentReadoutWidget::setValue, "segment readout");
    }
}

void SegmentReadoutWidget::setValue(double value)
{
    if (!std::isfinite(value))
    {
        return;
    }

    const int cells = static_cast<int>(_cfg.digits);
    QString rendered =
        QString::number(std::clamp(value, -1.0e9, 1.0e9), 'f', static_cast<int>(_cfg.decimals));

    // The prefix owns the leading cells and the value is pushed to the far end of
    // what remains, so the whole field is exactly `digits` cells wide and lines
    // up with the ghosts behind it.
    if (!_prefix.isEmpty())
    {
        const int room = cells - cellCount(_prefix);
        rendered = (cellCount(rendered) > room)
                       ? QString(std::max(0, room), kOverflow)
                       : QString(room - cellCount(rendered), QLatin1Char(' ')) + rendered;
        rendered.prepend(_prefix);
    }
    // A reading that needs more cells than the readout has cannot be shown
    // truthfully, so say so rather than drawing it over the neighbouring widget.
    else if (cellCount(rendered) > cells)
    {
        rendered = QString(cells, kOverflow);
    }

    if (rendered == _text)
    {
        // The drawn string changes far less often than the reading does.
        return;
    }
    _text = rendered;
    update();
}

void SegmentReadoutWidget::rebuildFontFor(const QSize& size)
{
    if (size == _font_size_for && _segment_font.pointSizeF() > 0.0)
    {
        return;
    }
    _font_size_for = size;

    // The caption takes a slice off the top or the side, so the segment cells
    // are fitted to what is left rather than to the whole widget.
    qreal available_width = size.width();
    qreal available_height = size.height();

    const bool has_caption = !_cfg.caption.empty();
    if (has_caption)
    {
        switch (_cfg.caption_position)
        {
            case SegmentCaptionPosition::top:
                available_height *= (1.0 - kCaptionFractionOfHeight - kCaptionGapFraction);
                break;
            case SegmentCaptionPosition::right:
                available_width *= 0.76;
                break;
        }
    }

    qreal low = kMinPointSize;
    qreal high = kMaxPointSize;
    for (int i = 0; i < kFitIterations; ++i)
    {
        const qreal mid = (low + high) / 2.0;
        QFont probe(_segment_family);
        probe.setPointSizeF(mid);
        const QFontMetricsF fm(probe);
        const QRectF box = fm.boundingRect(_ghost);
        if (box.width() <= available_width && fm.height() <= available_height)
        {
            low = mid;
        }
        else
        {
            high = mid;
        }
    }

    _segment_font = QFont(_segment_family);
    _segment_font.setPointSizeF(low);

    _caption_font = QFont(_caption_family);
    _caption_font.setPointSizeF(
        std::max<qreal>(6.0, size.height() * kCaptionFractionOfHeight * 0.7));
    _caption_font.setBold(true);
}

void SegmentReadoutWidget::paintEvent(QPaintEvent* /*event*/)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);

    rebuildFontFor(size());

    QRectF bounds(0, 0, width(), height());
    QRectF value_area = bounds;

    if (!_cfg.caption.empty())
    {
        p.setFont(_caption_font);
        p.setPen(dashboard::toQColor(_cfg.caption_color));
        const QString caption = QString::fromStdString(_cfg.caption);

        switch (_cfg.caption_position)
        {
            case SegmentCaptionPosition::top:
            {
                const qreal caption_height = bounds.height() * kCaptionFractionOfHeight;
                p.drawText(QRectF(bounds.left(), bounds.top(), bounds.width(), caption_height),
                           Qt::AlignHCenter | Qt::AlignVCenter, caption);
                value_area.setTop(bounds.top() + caption_height +
                                  bounds.height() * kCaptionGapFraction);
                break;
            }
            case SegmentCaptionPosition::right:
            {
                const qreal caption_width = bounds.width() * 0.24;
                p.drawText(QRectF(bounds.right() - caption_width, bounds.top(),
                                  caption_width, bounds.height()),
                           Qt::AlignLeft | Qt::AlignVCenter, caption);
                value_area.setRight(bounds.right() - caption_width);
                break;
            }
        }
    }

    p.setFont(_segment_font);

    // The ghosts first, then the value over them. Both are right-aligned in the
    // same box, which is what lines the two up cell for cell: DSEG's period has
    // zero width, so "53.60" advances the same four cells as "~~~~".
    if (_cfg.show_ghosts)
    {
        p.setPen(dashboard::toQColor(_cfg.ghost_color));
        p.drawText(value_area, Qt::AlignRight | Qt::AlignVCenter, _ghost);
    }

    p.setPen(dashboard::toQColor(_cfg.lit_color));
    p.drawText(value_area, Qt::AlignRight | Qt::AlignVCenter, _text);
}

#include "segment_readout/moc_segment_readout.cpp"
