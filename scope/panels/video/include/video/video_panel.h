#ifndef SCOPE_VIDEO_PANEL_H_
#define SCOPE_VIDEO_PANEL_H_

// data_source.h rather than a forward declaration of DataSource: this panel
// holds a SignalHandle and a RawBuffer by value, and both are declared there.
#include "scope/data_source.h"
#include "scope/panel.h"
#include "scope/panel_types.h"
#include "scope/raw_buffer.h"

#include "video/config.h"
#include "video/stats.h"
#include "video/video_decode_worker.h"

#include <QImage>
#include <QString>

#include <limits>
#include <memory>
#include <string_view>
#include <vector>

class QToolButton;

namespace scope
{

class DataSource;
class TimeBase;
class VideoScrubber;

// An encoded video stream, drawn at the shared time base's position.
//
// The mirror image of TimeSeriesPanel at the binding seam: a plot takes numeric
// FIELDS and declines whole topics, this takes a whole TOPIC and declines
// fields. Neither the signal browser, the drag plumbing nor the Add Signal
// dialog knows either of them exists -- both answer the same two questions about
// the same BindingCandidate.
//
// WHAT MAKES IT A SCOPE PANEL RATHER THAN A VIEWER: it never has a position of
// its own. The frame on screen is the one at TimeBase's playhead, so a value
// read off a plot under the shared cursor and the picture beside it are the same
// instant. The panel's own scrubber moves that shared playhead rather than
// anything local -- see video_scrubber.h.
//
// DECODING IS FORWARD-ONLY, WHICH IS THE WHOLE SHAPE OF THIS CLASS. There is no
// such thing as decoding the frame at t; there is only decoding from the last
// seek point up to t. So a seek re-runs the decoder over a GOP -- sixty-odd
// access units -- and this panel does not do that itself. It ships the GOP to
// VideoDecodeWorker and collects the picture on a later render tick, because a
// catch-up on the GUI thread is a stall as long as the GOP is, and rationing it
// into per-tick slices only turned one stall into a seek that took a quarter of
// a second to land. See video_decode_worker.h.
class VideoPanel : public Panel
{
    Q_OBJECT

  public:
    using config_t = VideoPanelConfig_t;
    using stats_t = VideoPanelStats_t;

    static constexpr panel_type_t kPanelType = panel_type_t::video;
    static constexpr std::string_view kFriendlyName = "Video";

    // U+25A3 WHITE SQUARE CONTAINING BLACK SMALL SQUARE. Picked for the reason
    // docs/scope.md gives: it has to be in the default font on every platform,
    // because a missing glyph renders as a blank box that reads as a broken
    // button rather than a plain one. The obvious alternatives are worse -- a
    // play triangle means "play", and the film/camera emoji are not in the
    // default UI font on Linux.
    static constexpr std::string_view kToolbarGlyph = "▣";

    // The schema this panel will take, as the registry spells it. Note the
    // capital P: scope/include/scope/panel.h:26 predicted this panel and
    // misspelled it "CarplayVideo", which would match nothing.
    static constexpr std::string_view kAcceptedSchema = "CarPlayVideo";

    VideoPanel(const config_t& cfg, DataSource& source,
               double history_seconds = 60.0, QWidget* parent = nullptr);
    ~VideoPanel() override;

    panel_type_t panelType() const override { return kPanelType; }
    bool acceptsBinding(const BindingCandidate& candidate) const override;
    bool addBinding(const BindingCandidate& candidate) override;

    // One stream, so at most one label and only index 0 is removable. Both are
    // still virtual calls rather than a special case in the window: a panel that
    // holds exactly one binding is not a panel that holds none.
    std::vector<QString> bindingLabels() const override;
    std::size_t unboundBindingCount() const override;
    bool removeBinding(std::size_t index) override;

    void setTimeBase(TimeBase* time_base) override;
    void setHistorySeconds(double seconds) override;
    void rebindTo(DataSource& source) override;
    QString title() const override;

    const config_t& getConfig() const { return cfg_; }
    void applyConfig(const config_t& cfg);
    stats_t stats() const;

    // The encoded access unit inside a buffered message: the CarPlayVideo
    // `data` field, not the capnp envelope carrying it.
    //
    // PUBLIC ONLY SO IT CAN BE TESTED, and it earned that. The panel used to
    // hand libavcodec the whole envelope, which decoded anyway -- the software
    // H.264 decoder scans for a start code and silently steps over whatever
    // precedes it -- so nothing looked wrong until a hardware decoder, which
    // does not, rejected every frame that was not a keyframe. A seam that a
    // decoder will paper over is a seam a test has to hold.
    //
    // False for a payload that is not a readable CarPlayVideo message.
    static bool accessUnitBytes(const std::vector<std::uint8_t>& payload,
                                std::vector<std::uint8_t>& out);

    // Drops the stream, so the panel can be pointed at another one.
    bool removeStream();

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    // Called on every render tick of the shared time base.
    void onFrame();

    // Hand the decoder thread the window it needs and the instant wanted out of
    // it. Cheap and non-blocking: the encoded units are copied only when the
    // window actually changes, and the request supersedes any still pending.
    void dispatchDecode(double position);

    void bindStream();
    void releaseStream();

    // Turns one CarPlayVideo message into the reserved flag bits, WITHOUT the
    // source learning what CarPlayVideo is. Runs on a zenoh RX thread for a live
    // source, so it reads the capnp header and nothing more.
    static std::uint32_t classify(std::span<const std::uint8_t> payload);

    // Consumer flag bits, above RawMessage::kFirstUserFlag so they can never
    // collide with a reserved one that gains a meaning later.
    static constexpr std::uint32_t kFlagH265 = RawMessage::kFirstUserFlag << 0;

    config_t cfg_;
    DataSource* source_;

    RawHandle handle_ = kInvalidRaw;
    std::shared_ptr<RawBuffer> buffer_;

    TimeBase* time_base_ = nullptr;

    VideoDecodeWorker worker_;

    // The picture on screen, and the instant it belongs to. OWNED HERE rather
    // than read back off the decoder: the decoder is on another thread and
    // reuses its buffer for the next frame.
    QImage image_;
    double frame_t_ = 0.0;

    VideoScrubber* scrubber_ = nullptr;
    QToolButton* play_button_ = nullptr;

    // Time of the last access unit HANDED TO THE WORKER, so a tick that only
    // advances a little ships one frame rather than the window again.
    //
    // A TIME rather than an index, because the buffer trims from the front on a
    // live source and every index would shift under it. -infinity means "nothing
    // shipped yet".
    double shipped_through_ = -std::numeric_limits<double>::infinity();

    // Identifies the window currently shipped: the buffer generation it came
    // from, and where its GOP starts. Either changing means the decoder has to
    // start again, so the whole window goes over rather than a tail.
    //
    // See RawBuffer::generation() for why the oldest timestamp cannot serve on
    // its own: it moves both when the window is replaced and when it is trimmed.
    std::uint64_t window_generation_ = 0;
    double window_start_t_ = 0.0;
    bool window_valid_ = false;

    // Bumped whenever the pair above changes. What the worker compares, so it
    // never has to know what a generation or a GOP is.
    std::uint64_t window_id_ = 0;

    // History size the scrubber's ticks were built from, so an idle tick costs
    // one comparison rather than a walk over the whole window.
    std::size_t seek_points_size_ = 0;

    // What the decoder was last asked for, so a backwards move is detectable
    // even when it stays inside one window.
    double last_position_ = 0.0;

    // Seek points currently buffered, handed to the scrubber. Rebuilt only when
    // the window changes rather than per frame.
    std::vector<double> seek_points_;

    bool bind_failed_ = false;
};

}  // namespace scope

#endif  // SCOPE_VIDEO_PANEL_H_
