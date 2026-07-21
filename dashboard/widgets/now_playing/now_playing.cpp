#include "now_playing/now_playing.h"

#include <QPainter>
#include <QFontMetrics>

#include <spdlog/spdlog.h>

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

QColor toQColor(const helpers::Color& c)
{
    return QColor(QString::fromStdString(c.value()));
}

}  // namespace

NowPlayingWidget::NowPlayingWidget(NowPlayingConfig_t cfg, QWidget* parent) :
    QWidget(parent),
    _cfg(std::move(cfg))
{
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

void NowPlayingWidget::paintEvent(QPaintEvent* /*event*/)
{
    QString title, artist, album, app;
    float duration = 0.0f;
    float elapsed = 0.0f;
    bool playing = false;
    QImage art;
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
    }

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);

    const QRectF bounds(0, 0, width(), height());

    if (title.isEmpty())
    {
        p.setPen(toQColor(_cfg.detail_color));
        p.drawText(bounds, Qt::AlignCenter, "Nothing playing");
        return;
    }

    // Album art occupies a square on the left when present.
    QRectF text_area = bounds;
    if (_cfg.show_album_art && !art.isNull())
    {
        const qreal side = bounds.height();
        const QRectF art_rect(bounds.left(), bounds.top(), side, side);
        p.drawImage(art_rect, art);
        text_area.setLeft(art_rect.right() + side * 0.08);
    }

    // Scale text to the widget the way the other widgets do.
    const qreal s = std::max<qreal>(0.4, text_area.height() / 90.0);
    const QString family = dashboard::loadResourceFont(":/fonts/futura.ttf", "Helvetica");

    QFont title_font(family);
    title_font.setPointSizeF(std::max<qreal>(8.0, 16.0 * s));
    title_font.setBold(true);
    QFont detail_font(family);
    detail_font.setPointSizeF(std::max<qreal>(6.0, 11.0 * s));

    const QFontMetricsF title_fm(title_font);
    const QFontMetricsF detail_fm(detail_font);

    qreal y = text_area.top() + title_fm.height() * 0.2;

    p.setFont(title_font);
    p.setPen(toQColor(_cfg.title_color));
    p.drawText(QRectF(text_area.left(), y, text_area.width(), title_fm.height()),
               Qt::AlignLeft | Qt::AlignVCenter,
               title_fm.elidedText(title, Qt::ElideRight, text_area.width()));
    y += title_fm.height();

    p.setFont(detail_font);
    p.setPen(toQColor(_cfg.detail_color));
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
        p.setBrush(toQColor(_cfg.accent_color));
        p.drawRoundedRect(filled, bar_h / 2.0, bar_h / 2.0);

        p.setPen(toQColor(_cfg.detail_color));
        const QRectF times(text_area.left(), track.bottom() + bar_h * 0.5,
                           text_area.width(), detail_fm.height());
        p.drawText(times, Qt::AlignLeft | Qt::AlignVCenter, secondsToClock(elapsed));
        p.drawText(times, Qt::AlignRight | Qt::AlignVCenter, secondsToClock(duration));
    }

    // A subtle paused indicator so a stale display is distinguishable.
    if (!playing)
    {
        p.setPen(toQColor(_cfg.detail_color));
        p.setFont(detail_font);
        p.drawText(bounds.adjusted(0, 0, -4, -2), Qt::AlignRight | Qt::AlignTop, "II");
    }
}

#include "now_playing/moc_now_playing.cpp"
