#ifndef NOW_PLAYING_WIDGET_H_
#define NOW_PLAYING_WIDGET_H_

#include "now_playing/config.h"
#include "dashboard/widget_types.h"

#include "pub_sub/zenoh_subscriber.h"
#include "carplay_nowplaying.capnp.h"

#include <QtWidgets/QWidget>
#include <QImage>
#include <QString>

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
    // Runs on the zenoh subscriber thread.
    void onNowPlaying(CarPlayNowPlaying::Reader reader);

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
    uint32_t _art_seq = 0;
    QImage _album_art;

    std::unique_ptr<pub_sub::ZenohTypedSubscriber<CarPlayNowPlaying>> _sub;
};

#endif  // NOW_PLAYING_WIDGET_H_
