#include "video/video_panel.h"

#include "scope/data_source.h"
#include "scope/time_base.h"

#include "video/video_scrubber.h"

#include "carplay_video.capnp.h"

#include <capnp/message.h>
#include <capnp/serialize.h>

#include <QPainter>
#include <QToolButton>
#include <QVBoxLayout>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace scope
{

namespace
{

// THERE IS NO DECODE BUDGET HERE ANY MORE, and its absence is the point.
//
// onFrame() used to decode inline, rationed to 10 ms per render tick, because a
// GOP catch-up is around sixty access units at roughly a millisecond each and
// doing one inline stalls the window for twice the render period. The ration
// kept the window painting, but it also meant a seek took seven ticks -- about a
// quarter of a second -- to land, which is exactly what made scrubbing here feel
// unlike a video player.
//
// Decoding belongs to VideoDecodeWorker now, on a thread of its own, where it
// runs flat out and competes with nothing. What is left here is handing it a
// window and collecting what came back.

// Enough for one GOP of a live stream plus slack. Live retention is the config's
// business; this is only the floor under it.
constexpr double kMinRetentionSeconds = 1.0;

constexpr const char* kBackground = "#0B0D10";
constexpr const char* kOverlayText = "#8A94A6";

}  // namespace

VideoPanel::VideoPanel(const config_t& cfg, DataSource& source, double history_seconds,
                       QWidget* parent)
    : Panel(parent), cfg_(cfg), source_(&source)
{
    setObjectName("video_panel");

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // The picture takes everything the scrubber does not, so the stretch goes on
    // the spacer rather than on a widget -- paintEvent draws straight onto this
    // panel rather than into a child label, which is what keeps QWidget::grab()
    // screenshots of the video working.
    layout->addStretch(1);

    auto* controls = new QHBoxLayout();
    controls->setContentsMargins(2, 0, 2, 2);
    controls->setSpacing(4);

    play_button_ = new QToolButton(this);
    play_button_->setObjectName("video_play");
    play_button_->setText("▶");
    play_button_->setToolTip(tr("Play or pause the shared time base"));
    play_button_->setAutoRaise(true);
    controls->addWidget(play_button_);

    scrubber_ = new VideoScrubber(this);
    controls->addWidget(scrubber_, 1);

    layout->addLayout(controls);

    connect(play_button_, &QToolButton::clicked, this,
            [this]()
            {
                if (time_base_ != nullptr)
                {
                    time_base_->setPlaying(!time_base_->playing());
                }
            });

    // The scrubber drives the SHARED clock, never a position of this panel's
    // own. See video_scrubber.h for why that is a rule rather than a choice.
    connect(scrubber_, &VideoScrubber::seekRequested, this,
            [this](double t)
            {
                if (time_base_ != nullptr)
                {
                    time_base_->seek(t);
                }
            });
    connect(scrubber_, &VideoScrubber::interactionChanged, this,
            [this](bool active)
            {
                if (time_base_ != nullptr)
                {
                    time_base_->setInteracting(active);
                }
            });
    connect(scrubber_, &VideoScrubber::cursorRequested, this,
            [this](std::optional<double> t)
            {
                if (time_base_ != nullptr)
                {
                    time_base_->setCursor(t);
                }
            });

    setHistorySeconds(history_seconds);
    bindStream();
}

VideoPanel::~VideoPanel()
{
    releaseStream();
}

// ------------------------------------------------------------------- binding

std::uint32_t VideoPanel::classify(std::span<const std::uint8_t> payload)
{
    // Runs on a zenoh RX thread for a live source, so it stays a header read.
    // capnp's reader does no copying and no allocation for scalar fields.
    std::uint32_t flags = 0;
    try
    {
        const auto words = kj::arrayPtr(reinterpret_cast<const capnp::word*>(payload.data()),
                                        payload.size() / sizeof(capnp::word));
        capnp::FlatArrayMessageReader reader(words);
        const CarPlayVideo::Reader video = reader.getRoot<CarPlayVideo>();

        if (video.getIsKeyframe())
        {
            flags |= RawMessage::kSeekPoint;
        }
        if (video.getIsConfig())
        {
            // kPreamble ONLY, never kSeekPoint, and this is not a detail.
            //
            // Parameter sets are not decodable on their own -- libavcodec
            // returns AVERROR_INVALIDDATA for them -- so a "seek point" at a
            // config message is a window a decoder can start at and produce
            // nothing from. Worse, the config sits IMMEDIATELY before the
            // keyframe it describes, so marking both made the config its own
            // one-message GOP: the window ran from the config to the very next
            // seek point, which was the keyframe one message later. The panel
            // then held 128 bytes, decoded zero frames, and kept displaying the
            // last picture from before the seek -- a stale frame, which looks
            // exactly like a seek that worked.
            //
            // As kPreamble alone it is carried into the window that starts at
            // the keyframe after it, which is what it is for.
            flags |= RawMessage::kPreamble;
        }
        if (video.getCodec() == CarPlayVideo::Codec::H265)
        {
            flags |= kFlagH265;
        }
    }
    catch (const kj::Exception&)
    {
        // A truncated or non-capnp payload. Tagged as nothing, which makes it a
        // message the decoder can neither start from nor be confused by --
        // rather than an exception crossing a zenoh callback, which is fatal.
    }
    return flags;
}

bool VideoPanel::acceptsBinding(const BindingCandidate& candidate) const
{
    // The exact inverse of the plot's test: a whole topic, not a field of one.
    if (!candidate.isTopicLevel())
    {
        return false;
    }
    if (candidate.schema_name != kAcceptedSchema)
    {
        return false;
    }

    // One stream per panel. A second would have to be composited or tabbed, and
    // both of those are a different panel rather than a bigger version of this
    // one -- so this declines rather than silently replacing what is bound.
    return cfg_.zenoh_key.empty();
}

bool VideoPanel::addBinding(const BindingCandidate& candidate)
{
    if (!acceptsBinding(candidate))
    {
        return false;
    }

    cfg_.zenoh_key = candidate.zenoh_key;
    releaseStream();
    bindStream();
    emit configChanged();
    return true;
}

std::vector<QString> VideoPanel::bindingLabels() const
{
    if (cfg_.zenoh_key.empty())
    {
        return {};
    }
    return {QString::fromStdString(cfg_.zenoh_key)};
}

bool VideoPanel::removeBinding(std::size_t index)
{
    return index == 0 && removeStream();
}

bool VideoPanel::removeStream()
{
    if (cfg_.zenoh_key.empty())
    {
        return false;
    }

    releaseStream();
    cfg_.zenoh_key.clear();
    worker_.reset();
    image_ = QImage();
    seek_points_.clear();
    window_valid_ = false;
    update();
    emit configChanged();
    return true;
}

void VideoPanel::bindStream()
{
    bind_failed_ = false;
    if (cfg_.zenoh_key.empty() || source_ == nullptr)
    {
        return;
    }

    buffer_ = std::make_shared<RawBuffer>(std::max(cfg_.retention_seconds, kMinRetentionSeconds),
                                          static_cast<std::size_t>(cfg_.max_buffer_bytes));

    handle_ = source_->bindRaw(cfg_.zenoh_key, pub_sub::schema_type_t::CarPlayVideo, buffer_,
                               &VideoPanel::classify);

    if (handle_ == kInvalidSignal)
    {
        // A definite no, surfaced in the overlay and in stats().bound rather
        // than left as a panel that simply never shows anything -- which is
        // indistinguishable from a publisher that has not started.
        bind_failed_ = true;
        SPDLOG_WARN("[scope/video] could not bind '{}'.", cfg_.zenoh_key);
    }

    worker_.setHardwareEnabled(cfg_.hardware_decode);
    worker_.reset();
    window_valid_ = false;
}

void VideoPanel::releaseStream()
{
    // AGAINST THE SOURCE THAT ISSUED IT, before any repoint. A handle means
    // nothing to a source that did not issue it, and rebindTo() below relies on
    // this being called while source_ is still the old one.
    if (handle_ != kInvalidSignal && source_ != nullptr)
    {
        source_->releaseRaw(handle_);
    }
    handle_ = kInvalidSignal;
    buffer_.reset();
}

void VideoPanel::rebindTo(DataSource& source)
{
    // THE ORDERING RULE. Release first, while the old source is still alive and
    // can honour it; only then repoint. The window destroys the old source after
    // this returns, precisely so the release has somewhere to go.
    releaseStream();
    source_ = &source;

    // Everything decoded belongs to the old source's epoch, and on a new one it
    // would be a picture stamped with a time that does not exist. reset()
    // BLOCKS until the decoder thread is idle, precisely so a picture already in
    // flight cannot arrive afterwards and be drawn as if it were the new one's.
    worker_.reset();
    image_ = QImage();
    seek_points_.clear();
    window_valid_ = false;

    bindStream();
    update();
}

// ---------------------------------------------------------------- time base

void VideoPanel::setTimeBase(TimeBase* time_base)
{
    if (time_base_ != nullptr)
    {
        disconnect(time_base_, nullptr, this, nullptr);
    }

    time_base_ = time_base;
    if (time_base_ == nullptr)
    {
        return;
    }

    connect(time_base_, &TimeBase::frame, this, &VideoPanel::onFrame);
    connect(time_base_, &TimeBase::changed, this, [this]() { update(); });
    connect(time_base_, &TimeBase::cursorMoved, this,
            [this]()
            {
                if (scrubber_ != nullptr)
                {
                    scrubber_->setTimeCursor(time_base_->cursor());
                }
            });
}

void VideoPanel::setHistorySeconds(double seconds)
{
    // The workspace's retention is a FLOOR here, not the value. Video is three
    // orders of magnitude more expensive per second than a plotted signal, so
    // the panel's own retention_seconds is what governs -- see config.h.
    static_cast<void>(seconds);

    if (buffer_)
    {
        buffer_->setBounds(std::max(cfg_.retention_seconds, kMinRetentionSeconds),
                           static_cast<std::size_t>(cfg_.max_buffer_bytes));
    }
}

// ------------------------------------------------------------------ decoding

// The ENCODED ACCESS UNIT out of a buffered message -- the `data` field of the
// CarPlayVideo message, not the message.
//
// THE BUFFER HOLDS WHOLE CAPNP MESSAGES. RawBuffer is schema-agnostic on
// purpose: it stores the bytes that came off the wire and leaves reading them to
// whoever bound it. So a payload here is a capnp envelope with an Annex-B access
// unit somewhere inside it, and the decoder must be handed the inside.
//
// THIS WAS THE BUG HARDWARE DECODE FOUND. The whole envelope used to go to
// libavcodec, and it decoded -- the software H.264 decoder scans for a start
// code and skips whatever precedes it, so the capnp header was silently stepped
// over and nobody ever saw a symptom. VideoToolbox does not do that: it took the
// keyframes, whose parameter sets let it build a session, and rejected every
// P-frame with AVERROR_UNKNOWN -- a panel showing one frame every two seconds.
// Relying on a decoder to skip bytes we should not have sent was always wrong;
// it just cost nothing until it did.
bool VideoPanel::accessUnitBytes(const std::vector<std::uint8_t>& payload,
                                 std::vector<std::uint8_t>& out)
{
    try
    {
        const auto words = kj::arrayPtr(reinterpret_cast<const capnp::word*>(payload.data()),
                                        payload.size() / sizeof(capnp::word));
        capnp::FlatArrayMessageReader reader(words);
        const capnp::Data::Reader data = reader.getRoot<CarPlayVideo>().getData();
        out.assign(data.begin(), data.end());
        return true;
    }
    catch (const kj::Exception&)
    {
        // Truncated or not capnp at all. Skipped rather than fed: the same
        // message was tagged as neither seek point nor preamble by classify(),
        // so it was never something the decoder could start from.
        return false;
    }
}

namespace
{

// The window a decoder needs in order to reach `position`: from the seek point
// at or before it, extended backwards over the preamble messages immediately in
// front of that.
//
// The backwards extension carries the parameter sets in. They are published just
// ahead of the keyframe they describe, and a window starting exactly at the
// keyframe leaves them one message behind -- which decodes to nothing at all.
// RecordedSource cuts its own windows the same way and for the same reason; this
// is the LIVE side of the same rule, where nothing has cut them for us.
bool gopStartFor(const RawHistory& history, double position, std::size_t& start)
{
    if (history.empty())
    {
        return false;
    }

    std::size_t at = history.lowerBound(position);
    while (at < history.size() && history[at].t <= position)
    {
        ++at;
    }
    if (at == 0)
    {
        // Everything buffered is later than the position asked for.
        return false;
    }
    --at;

    while ((history[at].flags & RawMessage::kSeekPoint) == 0)
    {
        if (at == 0)
        {
            // Nothing decodable at or before here. The honest answer -- it is
            // the state a live subscriber is in before its first keyframe.
            return false;
        }
        --at;
    }

    while (at > 0 && (history[at - 1].flags & RawMessage::kPreamble) != 0)
    {
        --at;
    }

    start = at;
    return true;
}

}  // namespace

void VideoPanel::dispatchDecode(double position)
{
    if (!buffer_)
    {
        return;
    }

    const RawHistory& history = buffer_->history();
    if (history.empty())
    {
        return;
    }

    // The scrubber's ticks follow the buffer's CONTENTS, which on a live source
    // grow every frame. Rebuilt only when something actually changed, so an idle
    // tick costs a size comparison.
    if (history.size() != seek_points_size_)
    {
        seek_points_size_ = history.size();
        seek_points_.clear();
        for (std::size_t i = 0; i < history.size(); ++i)
        {
            if ((history[i].flags & RawMessage::kSeekPoint) != 0)
            {
                seek_points_.push_back(history[i].t);
            }
        }
        if (scrubber_ != nullptr)
        {
            scrubber_->setSeekPoints(seek_points_);
        }
    }

    std::size_t start = 0;
    if (!gopStartFor(history, position, start))
    {
        return;
    }

    // WHICH WINDOW THIS IS, on two counts. The generation catches a buffer that
    // was REPLACED -- every seek over a recording refills it with a different
    // GOP -- and the start time catches a live stream simply reaching its next
    // keyframe. Either one means the decoder starts again, so the whole window
    // goes over rather than a tail of it.
    //
    // See RawBuffer::generation() for why the oldest timestamp cannot do this on
    // its own: it moves both when the window is replaced and when it is trimmed.
    const std::uint64_t generation = buffer_->generation();
    const double start_t = history[start].t;

    const bool new_window =
        !window_valid_ || generation != window_generation_ || start_t != window_start_t_;

    if (new_window)
    {
        window_generation_ = generation;
        window_start_t_ = start_t;
        window_valid_ = true;
        ++window_id_;
        shipped_through_ = -std::numeric_limits<double>::infinity();
    }

    VideoDecodeWorker::Request request;
    request.window_id = window_id_;
    request.replace = new_window;
    request.position = position;

    // WHAT KIND OF WINDOW THIS IS, which decides both how it is decoded and what
    // running out of units means.
    //
    // A recording's window is one GOP loaded whole -- nothing more is coming, so
    // a frame still inside a delayed decoder must be drained out, and frame
    // threading's five-fold speed-up on the catch-up is free. A live window
    // grows every tick and its picture has to line up with the traces beside it,
    // so neither applies.
    const bool seekable = source_ != nullptr && source_->caps().seekable;
    request.complete = seekable;
    request.seek_optimised = seekable;

    // ONLY WHAT THE WORKER HAS NOT SEEN. A live stream adds a frame per tick to
    // a window that is already a megabyte, so shipping the window every time
    // would copy that megabyte thirty times a second to deliver one frame of it.
    for (std::size_t i = start; i < history.size(); ++i)
    {
        const RawMessage& message = history[i];
        if (!new_window && message.t <= shipped_through_)
        {
            continue;
        }

        VideoDecodeWorker::Unit unit;
        if (!accessUnitBytes(message.payload, unit.data))
        {
            continue;
        }
        unit.t = message.t;
        unit.is_config = (message.flags & RawMessage::kPreamble) != 0;
        unit.is_keyframe = (message.flags & RawMessage::kSeekPoint) != 0 && !unit.is_config;
        unit.h265 = (message.flags & kFlagH265) != 0;
        request.units.push_back(std::move(unit));

        shipped_through_ = message.t;
    }

    // A request with no new units is still worth making: the POSITION moved, and
    // on a scrub backwards inside one window that is the entire change.
    worker_.request(std::move(request));
}

void VideoPanel::onFrame()
{
    if (time_base_ == nullptr)
    {
        return;
    }

    const double position = time_base_->viewEnd();

    if (buffer_)
    {
        buffer_->drain(position);
    }

    dispatchDecode(position);

    // Whatever the decoder thread finished since the last tick. Collected HERE
    // rather than delivered by a signal, so a picture lands on a render tick
    // like everything else the window draws -- see video_decode_worker.h.
    bool produced = false;
    VideoDecodeWorker::Result result;
    if (worker_.takeResult(result))
    {
        image_ = std::move(result.image);
        frame_t_ = result.frame_t;
        produced = true;
    }

    if (scrubber_ != nullptr)
    {
        const SourceCaps caps = source_ != nullptr ? source_->caps() : SourceCaps{};
        const auto [range_begin, range_end] = time_base_->availableRange();
        scrubber_->setExtent(range_begin, range_end);
        scrubber_->setPlayhead(position);
        scrubber_->setSeekable(caps.seekable);
        scrubber_->setVisible(cfg_.show_scrubber);
        play_button_->setVisible(cfg_.show_scrubber && caps.seekable);
        play_button_->setText(time_base_->playing() ? "❚❚" : "▶");
    }

    if (produced)
    {
        update();
    }
}

// ------------------------------------------------------------------ painting

void VideoPanel::paintEvent(QPaintEvent* /*event*/)
{
    QPainter painter(this);

    // The area above the controls row. The scrubber lays itself out; this just
    // avoids drawing underneath it.
    QRect area = rect();
    if (cfg_.show_scrubber && scrubber_ != nullptr && scrubber_->isVisible())
    {
        area.setBottom(scrubber_->geometry().top() - 1);
    }

    painter.fillRect(area, QColor(kBackground));

    const QImage& image = image_;
    if (!image.isNull() && area.width() > 0 && area.height() > 0)
    {
        // Aspect-preserving and letterboxed. Stretching would be a lie about
        // what the phone drew, and a scope exists to show what actually
        // happened.
        const double scale = std::min(static_cast<double>(area.width()) / image.width(),
                                      static_cast<double>(area.height()) / image.height());
        const int w = std::max(1, static_cast<int>(image.width() * scale));
        const int h = std::max(1, static_cast<int>(image.height() * scale));
        const QRect target(area.x() + (area.width() - w) / 2, area.y() + (area.height() - h) / 2,
                           w, h);

        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.drawImage(target, image);
        return;
    }

    // NEVER JUST BLACK. Every reason this panel has no picture looks the same on
    // screen and they are not the same problem, so the one that applies is
    // written on it.
    QString message;
    const VideoDecodeWorker::Snapshot decoding = worker_.snapshot();
    if (cfg_.zenoh_key.empty())
    {
        message = tr("Drop a video topic here");
    }
    else if (bind_failed_)
    {
        message = tr("Could not bind %1").arg(QString::fromStdString(cfg_.zenoh_key));
    }
    else if (decoding.stats.received == 0)
    {
        message = tr("Waiting for %1").arg(QString::fromStdString(cfg_.zenoh_key));
    }
    else if (!decoding.synced)
    {
        message = tr("Waiting for a keyframe (%1 dropped)")
                      .arg(decoding.stats.dropped_before_sync);
    }
    else
    {
        message = tr("Decoding…");
    }

    painter.setPen(QColor(kOverlayText));
    painter.drawText(area, Qt::AlignCenter, message);
}

// -------------------------------------------------------------------- config

void VideoPanel::applyConfig(const config_t& cfg)
{
    const bool key_changed = cfg.zenoh_key != cfg_.zenoh_key;
    cfg_ = cfg;

    if (key_changed)
    {
        releaseStream();
        worker_.reset();
        image_ = QImage();
        seek_points_.clear();
        window_valid_ = false;
        bindStream();
    }
    else if (buffer_)
    {
        buffer_->setBounds(std::max(cfg_.retention_seconds, kMinRetentionSeconds),
                           static_cast<std::size_t>(cfg_.max_buffer_bytes));
    }

    // Takes effect at the next decoder rather than now: reopening one mid-GOP
    // would throw away the reference frames the picture on screen is built from.
    worker_.setHardwareEnabled(cfg_.hardware_decode);

    update();
    emit configChanged();
}

QString VideoPanel::title() const
{
    if (!cfg_.title.empty())
    {
        return QString::fromStdString(cfg_.title);
    }
    return cfg_.zenoh_key.empty() ? tr("Video") : QString::fromStdString(cfg_.zenoh_key);
}

VideoPanel::stats_t VideoPanel::stats() const
{
    stats_t out;
    out.zenoh_key = cfg_.zenoh_key;
    out.bound = handle_ != kInvalidSignal;

    const VideoDecodeWorker::Snapshot decoding = worker_.snapshot();
    const VideoDecoder::Stats& decoded = decoding.stats;
    out.received = decoded.received;
    out.decoded = decoded.decoded;
    out.presented = decoded.presented;
    out.decoder = decoding.backend;
    out.dropped_before_sync = decoded.dropped_before_sync;
    out.decode_errors = decoded.decode_errors;
    out.convert_errors = decoded.convert_errors;
    out.synced = decoding.synced;

    if (buffer_)
    {
        const RawHistory& history = buffer_->history();
        out.buffered = history.size();
        out.bytes = history.bytes();
        out.dropped_messages = buffer_->dropped();

        for (std::size_t i = 0; i < history.size(); ++i)
        {
            if ((history[i].flags & RawMessage::kSeekPoint) != 0)
            {
                ++out.keyframes;
            }
        }

        if (!history.empty())
        {
            out.has_data = true;
            out.t_first = history.oldest().t;
            out.t_last = history.newest().t;
        }
    }

    const QImage& image = image_;
    if (!image.isNull())
    {
        out.has_frame = true;
        out.frame_t = frame_t_;
        out.frame_width = static_cast<std::uint64_t>(image.width());
        out.frame_height = static_cast<std::uint64_t>(image.height());
    }

    return out;
}

}  // namespace scope
