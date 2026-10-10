#ifndef NOW_PLAYING_WIDGET_H_
#define NOW_PLAYING_WIDGET_H_

#include "now_playing/config.h"
#include "dashboard/widget_types.h"

#include "dashboard/typed_subscription.h"
#include "carplay_nowplaying.capnp.h"
#include "carplay_call.capnp.h"

#include <QtWidgets/QWidget>
#include <QFont>
#include <QFontMetricsF>
#include <QImage>
#include <QPixmap>
#include <QSize>
#include <QString>
#include <QTimer>
#include <QVariantAnimation>

#include <memory>
#include <string_view>

// Supplemental CarPlay widget: shows what the phone is playing. Subscribes to
// the driver node's metadata topic only -- no USB/AirPlay knowledge here.
class NowPlayingWidget : public QWidget
{
    Q_OBJECT

  public:
    using config_t = NowPlayingConfig_t;
    static constexpr std::string_view kFriendlyName = "Now Playing";
    static constexpr widget_type_t kWidgetType = widget_type_t::now_playing;

    NowPlayingWidget(NowPlayingConfig_t cfg, QWidget* parent = nullptr);
    ~NowPlayingWidget();
    const config_t& getConfig() const { return _cfg; }

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    // Sentinel rather than 0: the artwork is only decoded when the sequence
    // changes, and a publisher whose first artwork carries seq 0 -- a fresh
    // process, or one that never sets the field -- matched the initial value and
    // had its artwork dropped forever.
    static constexpr uint32_t kNoArtSeq = UINT32_MAX;

    // One now-playing message, copied out on the zenoh thread. The artwork is
    // decoded there too, once per sequence number, so the GUI never blocks on
    // an image decode.
    struct Track
    {
        QString title;
        QString artist;
        QString album;
        QString app;
        float duration_sec = 0.0f;
        float elapsed_sec = 0.0f;
        bool playing = false;
        uint32_t art_seq = kNoArtSeq;
        QImage album_art;
    };

    struct Call
    {
        CarPlayCall::State state = CarPlayCall::State::IDLE;
        QString name;
        QString number;
        float duration_sec = 0.0f;
    };

    // Qt thread: the latest snapshot of each, delivered coalesced.
    void setTrack(Track track);
    void setCall(Call call);

    // Qt thread. Starts the fade towards `to_call` if it is not already headed
    // there, and arms the linger timer when a call has just ended.
    void driveTransition(bool to_call);

    void paintMusic(QPainter& p, const QRectF& bounds);
    void paintCall(QPainter& p, const QRectF& bounds);

    NowPlayingConfig_t _cfg;

    // GUI thread only; written by the delivery tick, read by paint.
    Track _track;
    Call _call;

    // Paint-path caches. None of this belongs in paintEvent: the font family
    // came from a QFontDatabase registration on every repaint, and the fonts,
    // their metrics and the scaled artwork all derive from things that change
    // far more slowly than the frame rate.
    QString _font_family;
    void rebuildFontsFor(qreal scale);
    qreal _font_scale = -1.0;
    QFont _title_font;
    QFont _detail_font;
    std::unique_ptr<QFontMetricsF> _title_fm;
    std::unique_ptr<QFontMetricsF> _detail_fm;

    // Artwork scaled to the box it is drawn in, keyed on both the box and which
    // artwork it is.
    QPixmap _scaled_art;
    QSize _scaled_art_size;
    uint32_t _scaled_art_seq = kNoArtSeq;

    // 0 is fully music, 1 is fully call. Qt thread only -- the animation that
    // drives it and the paint that reads it both run there, so it needs no lock.
    qreal _call_mix = 0.0;
    // What the animation is currently heading towards, so a repeated update for
    // an unchanged call state does not restart the fade on every sample.
    bool _showing_call = false;
    QVariantAnimation _transition;
    // Holds the call face up for call_linger_ms after the phone hangs up.
    QTimer _linger;

    dashboard::TypedSubscriptionPtr<CarPlayNowPlaying, Track> _sub;
    dashboard::TypedSubscriptionPtr<CarPlayCall, Call> _call_sub;
};

#endif  // NOW_PLAYING_WIDGET_H_
