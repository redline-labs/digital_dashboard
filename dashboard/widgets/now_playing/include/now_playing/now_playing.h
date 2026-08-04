#ifndef NOW_PLAYING_WIDGET_H_
#define NOW_PLAYING_WIDGET_H_

#include "now_playing/config.h"
#include "dashboard/widget_types.h"

#include "pub_sub/zenoh_subscriber.h"
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
#include <mutex>
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
    // Both run on zenoh subscriber threads.
    void onNowPlaying(CarPlayNowPlaying::Reader reader);
    void onCall(CarPlayCall::Reader reader);

    // Qt thread. Starts the fade towards `to_call` if it is not already headed
    // there, and arms the linger timer when a call has just ended.
    void driveTransition(bool to_call);

    void paintMusic(QPainter& p, const QRectF& bounds);
    void paintCall(QPainter& p, const QRectF& bounds);

    NowPlayingConfig_t _cfg;

    // Guards everything below; written by the subscriber thread, read by paint.
    std::mutex _mutex;
    QString _title;
    QString _artist;
    QString _album;
    QString _app;
    float _duration_sec = 0.0f;
    float _elapsed_sec = 0.0f;
    bool _playing = false;
    // Sentinel rather than 0: the artwork is only decoded when the sequence
    // changes, and a publisher whose first artwork carries seq 0 -- a fresh
    // process, or one that never sets the field -- matched the initial value and
    // had its artwork dropped forever.
    static constexpr uint32_t kNoArtSeq = UINT32_MAX;
    uint32_t _art_seq = kNoArtSeq;
    QImage _album_art;

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

    // ---- Call takeover -----------------------------------------------------
    // Guarded by _mutex like the media fields above; written by the call
    // subscriber thread, read by paint.
    CarPlayCall::State _call_state = CarPlayCall::State::IDLE;
    QString _call_name;
    QString _call_number;
    float _call_duration_sec = 0.0f;

    // 0 is fully music, 1 is fully call. Qt thread only -- the animation that
    // drives it and the paint that reads it both run there, so it needs no lock.
    qreal _call_mix = 0.0;
    // What the animation is currently heading towards, so a repeated update for
    // an unchanged call state does not restart the fade on every sample.
    bool _showing_call = false;
    QVariantAnimation _transition;
    // Holds the call face up for call_linger_ms after the phone hangs up.
    QTimer _linger;

    std::unique_ptr<pub_sub::ZenohTypedSubscriber<CarPlayNowPlaying>> _sub;
    std::unique_ptr<pub_sub::ZenohTypedSubscriber<CarPlayCall>> _call_sub;
};

#endif  // NOW_PLAYING_WIDGET_H_
