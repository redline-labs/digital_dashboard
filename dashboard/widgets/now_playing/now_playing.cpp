#include "now_playing/now_playing.h"

#include <QPainter>
#include <QFontMetrics>

#include <spdlog/spdlog.h>

#include "qt_helpers/widget_colors.h"
#include "qt_helpers/widget_fonts.h"

namespace
{

QString secondsToClock(float seconds)
{
    if (seconds < 0.0f)
    {
        seconds = 0.0f;
    }
    const int total = static_cast<int>(seconds);
    return QString("%1:%2").arg(total / 60).arg(total % 60, 2, 10, QChar('0'));
}

// The states in which a call owns the widget. `disconnected` counts: it is what
// the phone reports on hang-up, and the linger timer is what eventually takes
// the widget back to the music.
bool callIsLive(CarPlayCall::State state)
{
    switch (state)
    {
        case CarPlayCall::State::INCOMING:
        case CarPlayCall::State::DIALING:
        case CarPlayCall::State::ACTIVE:
        case CarPlayCall::State::HELD:
        case CarPlayCall::State::DISCONNECTED:
            return true;
        case CarPlayCall::State::IDLE:
            return false;
    }
    return false;
}

// True once the call is over and only the linger is keeping the face up.
bool callHasEnded(CarPlayCall::State state)
{
    return state == CarPlayCall::State::DISCONNECTED;
}

QString callStatusLine(CarPlayCall::State state, float duration_sec)
{
    switch (state)
    {
        case CarPlayCall::State::INCOMING:     return QStringLiteral("Incoming call");
        case CarPlayCall::State::DIALING:      return QStringLiteral("Dialling…");
        case CarPlayCall::State::ACTIVE:       return secondsToClock(duration_sec);
        case CarPlayCall::State::HELD:         return QStringLiteral("On hold");
        case CarPlayCall::State::DISCONNECTED: return QStringLiteral("Call ended");
        case CarPlayCall::State::IDLE:         return QString();
    }
    return QString();
}

}  // namespace

NowPlayingWidget::NowPlayingWidget(NowPlayingConfig_t cfg, QWidget* parent) :
    QWidget(parent),
    _cfg(std::move(cfg))
{
    // Resolved once, here, like every other widget. paintEvent used to call this
    // per repaint, which re-registered the font with QFontDatabase each time.
    _font_family = qt_helpers::loadResourceFont(":/fonts/futura.ttf", "Helvetica");

    // The cross-fade and the linger both live on the Qt thread; the zenoh
    // callbacks only ever post to them.
    _transition.setDuration(_cfg.transition_ms);
    _transition.setEasingCurve(QEasingCurve::InOutCubic);
    connect(&_transition, &QVariantAnimation::valueChanged, this,
            [this](const QVariant& value) {
                _call_mix = value.toReal();
                update();
            });

    _linger.setSingleShot(true);
    _linger.setInterval(_cfg.call_linger_ms);
    // Fires once the hung-up call has been on screen long enough; that is what
    // actually hands the widget back to the music.
    connect(&_linger, &QTimer::timeout, this, [this] { driveTransition(false); });

    _sub = std::make_unique<pub_sub::ZenohTypedSubscriber<CarPlayNowPlaying>>(
        _cfg.zenoh_key,
        [this](CarPlayNowPlaying::Reader reader) { onNowPlaying(reader); });

    if (_cfg.show_calls)
    {
        _call_sub = std::make_unique<pub_sub::ZenohTypedSubscriber<CarPlayCall>>(
            _cfg.call_zenoh_key,
            [this](CarPlayCall::Reader reader) { onCall(reader); });
    }
}

NowPlayingWidget::~NowPlayingWidget()
{
    // Drop the subscribers first so their callbacks cannot race destruction.
    _sub.reset();
    _call_sub.reset();
}

void NowPlayingWidget::onNowPlaying(CarPlayNowPlaying::Reader reader)
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _title = QString::fromStdString(reader.getTitle());
        _artist = QString::fromStdString(reader.getArtist());
        _album = QString::fromStdString(reader.getAlbum());
        _app = QString::fromStdString(reader.getApp());
        _duration_sec = reader.getDurationSec();
        _elapsed_sec = reader.getElapsedSec();
        _playing = reader.getPlaying();

        // Artwork only arrives on track change; keep the previous image when
        // the sequence is unchanged so we don't decode on every update.
        const uint32_t seq = reader.getAlbumArtSeq();
        auto art = reader.getAlbumArt();
        if (seq != _art_seq)
        {
            _art_seq = seq;
            _album_art = QImage{};
            if (art.size() > 0 && !_album_art.loadFromData(art.begin(), static_cast<int>(art.size())))
            {
                SPDLOG_WARN("[now_playing] failed to decode {} bytes of album art (seq {})", art.size(), seq);
            }
        }
    }

    QMetaObject::invokeMethod(this, [this] { update(); }, Qt::QueuedConnection);
}

void NowPlayingWidget::onCall(CarPlayCall::Reader reader)
{
    CarPlayCall::State state = CarPlayCall::State::IDLE;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _call_state = reader.getState();
        _call_name = QString::fromStdString(reader.getRemoteName());
        _call_number = QString::fromStdString(reader.getRemoteNumber());
        _call_duration_sec = reader.getDurationSec();
        state = _call_state;
    }

    // Hop to the Qt thread: driveTransition touches a QVariantAnimation and a
    // QTimer, and neither may be driven from a zenoh callback thread.
    QMetaObject::invokeMethod(
        this,
        [this, state] {
            driveTransition(callIsLive(state));

            // A hang-up keeps the face up for the linger and then reverts. An
            // outright idle -- the phone dropping straight back to no call --
            // reverts immediately, so cancel any linger already running.
            if (callHasEnded(state))
            {
                _linger.start();
            }
            else
            {
                _linger.stop();
            }
            update();
        },
        Qt::QueuedConnection);
}

void NowPlayingWidget::driveTransition(bool to_call)
{
    if (to_call == _showing_call)
    {
        // Already there or already on the way. Restarting the animation on every
        // sample of an unchanged call state would freeze the fade at its first
        // frame for as long as the publisher keeps talking.
        return;
    }
    _showing_call = to_call;

    const qreal target = to_call ? 1.0 : 0.0;
    if (_cfg.transition_ms == 0)
    {
        _transition.stop();
        _call_mix = target;
        update();
        return;
    }

    _transition.stop();
    _transition.setStartValue(_call_mix);
    _transition.setEndValue(target);
    _transition.start();
}

// Rebuilds the fonts and their metrics for a new scale. Cheap to call every
// frame; it does nothing unless the scale actually moved.
void NowPlayingWidget::rebuildFontsFor(qreal scale)
{
    if (_title_fm && _detail_fm && qFuzzyCompare(scale, _font_scale))
    {
        return;
    }

    _font_scale = scale;

    _title_font = QFont(_font_family);
    _title_font.setPointSizeF(std::max<qreal>(8.0, 16.0 * scale));
    _title_font.setBold(true);

    _detail_font = QFont(_font_family);
    _detail_font.setPointSizeF(std::max<qreal>(6.0, 11.0 * scale));

    // QFontMetricsF has no default constructor, hence the indirection.
    _title_fm = std::make_unique<QFontMetricsF>(_title_font);
    _detail_fm = std::make_unique<QFontMetricsF>(_detail_font);
}

void NowPlayingWidget::paintEvent(QPaintEvent* /*event*/)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);

    const QRectF bounds(0, 0, width(), height());

    // The two faces cross-fade and slide past each other: the outgoing one
    // leaves upwards, the incoming one arrives from below. Painting both at
    // partial opacity for the duration is what makes it read as one panel
    // changing its mind rather than two widgets swapping.
    const qreal mix = std::clamp<qreal>(_call_mix, 0.0, 1.0);
    const qreal slide = bounds.height() * 0.35;

    if (mix < 1.0)
    {
        p.save();
        p.setOpacity(1.0 - mix);
        p.translate(0.0, -slide * mix);
        paintMusic(p, bounds);
        p.restore();
    }

    if (mix > 0.0)
    {
        p.save();
        p.setOpacity(mix);
        p.translate(0.0, slide * (1.0 - mix));
        paintCall(p, bounds);
        p.restore();
    }
}

void NowPlayingWidget::paintCall(QPainter& p, const QRectF& bounds)
{
    CarPlayCall::State state = CarPlayCall::State::IDLE;
    QString name, number;
    float duration = 0.0f;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        state = _call_state;
        name = _call_name;
        number = _call_number;
        duration = _call_duration_sec;
    }

    const qreal s = std::max<qreal>(0.4, bounds.height() / 90.0);
    rebuildFontsFor(s);
    const QFontMetricsF& title_fm = *_title_fm;
    const QFontMetricsF& detail_fm = *_detail_fm;

    const QColor accent = qt_helpers::toQColor(_cfg.call_accent_color);

    // A caller badge occupies the same square the album art does, so the two
    // faces line up through the fade instead of the text jumping sideways.
    QRectF text_area = bounds;
    const qreal side = bounds.height();
    const QRectF badge_rect(bounds.left(), bounds.top(), side, side);
    const qreal inset = side * 0.16;
    const QRectF circle = badge_rect.adjusted(inset, inset, -inset, -inset);

    p.setPen(Qt::NoPen);
    // Dim for a call that is over, so "Call ended" does not look like a live one.
    p.setBrush(callHasEnded(state) ? accent.darker(220) : accent);
    p.drawEllipse(circle);

    // The caller's initial, or a handset glyph when there is no name to take one
    // from (an unknown number, which is exactly when there is no initial).
    const QString display_name = name.isEmpty() ? number : name;
    QFont badge_font(_title_font);
    badge_font.setPointSizeF(std::max<qreal>(10.0, 22.0 * s));
    p.setFont(badge_font);
    p.setPen(QColor(20, 20, 20));
    p.drawText(circle, Qt::AlignCenter,
               name.isEmpty() ? QStringLiteral("☎") : name.left(1).toUpper());

    text_area.setLeft(badge_rect.right() + side * 0.08);

    qreal y = text_area.top() + title_fm.height() * 0.2;

    p.setFont(_title_font);
    p.setPen(qt_helpers::toQColor(_cfg.title_color));
    p.drawText(QRectF(text_area.left(), y, text_area.width(), title_fm.height()),
               Qt::AlignLeft | Qt::AlignVCenter,
               title_fm.elidedText(display_name.isEmpty() ? QStringLiteral("Unknown caller")
                                                          : display_name,
                                   Qt::ElideRight, text_area.width()));
    y += title_fm.height();

    p.setFont(_detail_font);

    // Only worth a second line when the first one was the name.
    if (!name.isEmpty() && !number.isEmpty())
    {
        p.setPen(qt_helpers::toQColor(_cfg.detail_color));
        p.drawText(QRectF(text_area.left(), y, text_area.width(), detail_fm.height()),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   detail_fm.elidedText(number, Qt::ElideRight, text_area.width()));
        y += detail_fm.height();
    }

    p.setPen(callHasEnded(state) ? qt_helpers::toQColor(_cfg.detail_color) : accent);
    p.drawText(QRectF(text_area.left(), y, text_area.width(), detail_fm.height()),
               Qt::AlignLeft | Qt::AlignVCenter, callStatusLine(state, duration));
}

void NowPlayingWidget::paintMusic(QPainter& p, const QRectF& bounds)
{
    QString title, artist, album, app;
    float duration = 0.0f;
    float elapsed = 0.0f;
    bool playing = false;
    QImage art;
    uint32_t art_seq = kNoArtSeq;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        title = _title;
        artist = _artist;
        album = _album;
        app = _app;
        duration = _duration_sec;
        elapsed = _elapsed_sec;
        playing = _playing;
        art = _album_art;
        art_seq = _art_seq;
    }

    if (title.isEmpty())
    {
        p.setPen(qt_helpers::toQColor(_cfg.detail_color));
        p.drawText(bounds, Qt::AlignCenter, "Nothing playing");
        return;
    }

    // Album art occupies a square on the left when present. Scaling the
    // full-resolution image with SmoothPixmapTransform is the single most
    // expensive thing on this path, and the result only changes when the box or
    // the artwork does -- so keep the scaled copy.
    QRectF text_area = bounds;
    if (_cfg.show_album_art && !art.isNull())
    {
        const qreal side = bounds.height();
        const QRectF art_rect(bounds.left(), bounds.top(), side, side);
        const QSize target = art_rect.size().toSize();

        if (_scaled_art_size != target || _scaled_art_seq != art_seq || _scaled_art.isNull())
        {
            _scaled_art = QPixmap::fromImage(
                art.scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation));
            _scaled_art_size = target;
            _scaled_art_seq = art_seq;
        }

        p.drawPixmap(art_rect.topLeft(), _scaled_art);
        text_area.setLeft(art_rect.right() + side * 0.08);
    }

    // Scale text to the widget the way the other widgets do. The fonts and their
    // metrics derive only from this number, so they are rebuilt when it moves
    // rather than on every frame -- this used to re-register the TTF with
    // QFontDatabase per repaint.
    const qreal s = std::max<qreal>(0.4, text_area.height() / 90.0);
    rebuildFontsFor(s);

    const QFont& title_font = _title_font;
    const QFont& detail_font = _detail_font;
    const QFontMetricsF& title_fm = *_title_fm;
    const QFontMetricsF& detail_fm = *_detail_fm;

    // The text stacks downwards from the top and the progress block is anchored
    // to the bottom, so the two have to be told about each other. Reserve the
    // block's height first and stop the text at it: without this, a track with a
    // title, an artist AND an album drew the album line straight through the
    // progress track.
    const bool show_progress = _cfg.show_progress && duration > 0.0f;
    const qreal bar_h = std::max<qreal>(2.0, 4.0 * s);
    // Matches the block laid out below -- one bar_h of clearance, the bar, half a
    // bar_h of gap, then the elapsed/duration line.
    const qreal progress_h = show_progress ? detail_fm.height() + bar_h * 3.0 : 0.0;
    const qreal text_bottom = text_area.bottom() - progress_h;

    qreal y = text_area.top() + title_fm.height() * 0.2;

    p.setFont(title_font);
    p.setPen(qt_helpers::toQColor(_cfg.title_color));
    p.drawText(QRectF(text_area.left(), y, text_area.width(), title_fm.height()),
               Qt::AlignLeft | Qt::AlignVCenter,
               title_fm.elidedText(title, Qt::ElideRight, text_area.width()));
    y += title_fm.height();

    p.setFont(detail_font);
    p.setPen(qt_helpers::toQColor(_cfg.detail_color));
    for (const QString& line : {artist, album})
    {
        if (line.isEmpty())
        {
            continue;
        }
        // Dropping the line is the honest failure here. Squeezing it in would put
        // it over the progress bar, and shrinking the font to fit would leave the
        // album in unreadable type nobody asked for.
        if (y + detail_fm.height() > text_bottom)
        {
            break;
        }
        p.drawText(QRectF(text_area.left(), y, text_area.width(), detail_fm.height()),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   detail_fm.elidedText(line, Qt::ElideRight, text_area.width()));
        y += detail_fm.height();
    }

    if (show_progress)
    {
        const qreal bar_y = text_area.bottom() - detail_fm.height() - bar_h * 2.0;
        const QRectF track(text_area.left(), bar_y, text_area.width(), bar_h);

        p.setPen(Qt::NoPen);
        p.setBrush(QColor(70, 70, 70));
        p.drawRoundedRect(track, bar_h / 2.0, bar_h / 2.0);

        const qreal fraction = std::clamp<qreal>(elapsed / duration, 0.0, 1.0);
        QRectF filled = track;
        filled.setWidth(track.width() * fraction);
        p.setBrush(qt_helpers::toQColor(_cfg.accent_color));
        p.drawRoundedRect(filled, bar_h / 2.0, bar_h / 2.0);

        p.setPen(qt_helpers::toQColor(_cfg.detail_color));
        const QRectF times(text_area.left(), track.bottom() + bar_h * 0.5,
                           text_area.width(), detail_fm.height());
        p.drawText(times, Qt::AlignLeft | Qt::AlignVCenter, secondsToClock(elapsed));
        p.drawText(times, Qt::AlignRight | Qt::AlignVCenter, secondsToClock(duration));
    }

    // A subtle paused indicator so a stale display is distinguishable.
    if (!playing)
    {
        p.setPen(qt_helpers::toQColor(_cfg.detail_color));
        p.setFont(detail_font);
        p.drawText(bounds.adjusted(0, 0, -4, -2), Qt::AlignRight | Qt::AlignTop, "II");
    }
}

#include "now_playing/moc_now_playing.cpp"
