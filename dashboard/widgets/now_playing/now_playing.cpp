#include "now_playing/now_playing.h"

#include <QPainter>
#include <QFontMetrics>

#include <spdlog/spdlog.h>

#include "dashboard/widget_colors.h"
#include "dashboard/widget_fonts.h"

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

}  // namespace

NowPlayingWidget::NowPlayingWidget(NowPlayingConfig_t cfg, QWidget* parent) :
    QWidget(parent),
    _cfg(std::move(cfg))
{
    // Resolved once, here, like every other widget. paintEvent used to call this
    // per repaint, which re-registered the font with QFontDatabase each time.
    _font_family = dashboard::loadResourceFont(":/fonts/futura.ttf", "Helvetica");

    _sub = std::make_unique<pub_sub::ZenohTypedSubscriber<CarPlayNowPlaying>>(
        _cfg.zenoh_key,
        [this](CarPlayNowPlaying::Reader reader) { onNowPlaying(reader); });
}

NowPlayingWidget::~NowPlayingWidget()
{
    // Drop the subscriber first so its callback cannot race destruction.
    _sub.reset();
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

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);

    const QRectF bounds(0, 0, width(), height());

    if (title.isEmpty())
    {
        p.setPen(dashboard::toQColor(_cfg.detail_color));
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

    qreal y = text_area.top() + title_fm.height() * 0.2;

    p.setFont(title_font);
    p.setPen(dashboard::toQColor(_cfg.title_color));
    p.drawText(QRectF(text_area.left(), y, text_area.width(), title_fm.height()),
               Qt::AlignLeft | Qt::AlignVCenter,
               title_fm.elidedText(title, Qt::ElideRight, text_area.width()));
    y += title_fm.height();

    p.setFont(detail_font);
    p.setPen(dashboard::toQColor(_cfg.detail_color));
    for (const QString& line : {artist, album})
    {
        if (line.isEmpty())
        {
            continue;
        }
        p.drawText(QRectF(text_area.left(), y, text_area.width(), detail_fm.height()),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   detail_fm.elidedText(line, Qt::ElideRight, text_area.width()));
        y += detail_fm.height();
    }

    if (_cfg.show_progress && duration > 0.0f)
    {
        const qreal bar_h = std::max<qreal>(2.0, 4.0 * s);
        const qreal bar_y = text_area.bottom() - detail_fm.height() - bar_h * 2.0;
        const QRectF track(text_area.left(), bar_y, text_area.width(), bar_h);

        p.setPen(Qt::NoPen);
        p.setBrush(QColor(70, 70, 70));
        p.drawRoundedRect(track, bar_h / 2.0, bar_h / 2.0);

        const qreal fraction = std::clamp<qreal>(elapsed / duration, 0.0, 1.0);
        QRectF filled = track;
        filled.setWidth(track.width() * fraction);
        p.setBrush(dashboard::toQColor(_cfg.accent_color));
        p.drawRoundedRect(filled, bar_h / 2.0, bar_h / 2.0);

        p.setPen(dashboard::toQColor(_cfg.detail_color));
        const QRectF times(text_area.left(), track.bottom() + bar_h * 0.5,
                           text_area.width(), detail_fm.height());
        p.drawText(times, Qt::AlignLeft | Qt::AlignVCenter, secondsToClock(elapsed));
        p.drawText(times, Qt::AlignRight | Qt::AlignVCenter, secondsToClock(duration));
    }

    // A subtle paused indicator so a stale display is distinguishable.
    if (!playing)
    {
        p.setPen(dashboard::toQColor(_cfg.detail_color));
        p.setFont(detail_font);
        p.drawText(bounds.adjusted(0, 0, -4, -2), Qt::AlignRight | Qt::AlignTop, "II");
    }
}

#include "now_playing/moc_now_playing.cpp"
