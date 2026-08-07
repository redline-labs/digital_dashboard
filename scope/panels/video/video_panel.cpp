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

// How long onFrame() may spend decoding before giving the event loop a turn.
//
// A GOP catch-up after a seek is around sixty access units at roughly a
// millisecond each, so decoding one inline is a ~60 ms stall -- twice the render
// period, on every scrub tick of a drag. Budgeting it spreads the catch-up over
// two or three ticks instead, which is a picture that lags the scrubber slightly
// rather than a window that stops repainting.
constexpr double kDecodeBudgetMs = 10.0;

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

bool VideoPanel::removeStream()
{
    if (cfg_.zenoh_key.empty())
    {
        return false;
    }

    releaseStream();
    cfg_.zenoh_key.clear();
    decoder_.reset();
    decoder_.resetStats();
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

    decoder_.reset();
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
    // would be a picture stamped with a time that does not exist.
    decoder_.reset();
    decoder_.resetStats();
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

bool VideoPanel::decodeUpTo(double position)
{
    if (!buffer_)
    {
        return false;
    }

    const RawHistory& history = buffer_->history();
    if (history.empty())
    {
        return false;
    }

    // PROGRESS IS TRACKED BY TIME, NOT BY INDEX, and the window is identified by
    // the buffer's generation rather than by its oldest timestamp. Both were
    // indices/timestamps once, and both are wrong on a LIVE source for the same
    // reason: the buffer trims its front once retention is reached, which shifts
    // every index and moves the oldest timestamp. The panel would then reset its
    // decoder and re-decode the whole retention window on every tick, for ever,
    // starting exactly when retention first fills -- invisible in a short run.
    const std::uint64_t generation = buffer_->generation();

    bool restart = !window_valid_ || generation != window_generation_;

    if (!restart && position < last_position_)
    {
        // Moved backwards inside the window we already have. The buffer is
        // right; the DECODER is not, because it cannot un-consume the frames it
        // has already been given. Start it again from the front of the window.
        restart = true;
    }

    if (restart)
    {
        window_generation_ = generation;
        window_valid_ = true;
        decoder_.reset();
        decoded_through_ = -std::numeric_limits<double>::infinity();
    }

    last_position_ = position;

    // The scrubber's ticks follow the buffer's CONTENTS, which on a live source
    // grow every frame. Rebuilt only when something actually changed, so an idle
    // tick costs a size comparison.
    if (restart || history.size() != seek_points_size_)
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

    // Everything after what has already been fed, up to and including the
    // playhead. Closed at the top, the same convention RecordedSource::refill
    // uses, so a frame stamped exactly at the position is shown rather than the
    // one before it.
    std::size_t index = 0;
    if (decoded_through_ > -std::numeric_limits<double>::infinity())
    {
        index = history.lowerBound(decoded_through_);
        while (index < history.size() && history[index].t <= decoded_through_)
        {
            ++index;
        }
    }

    const auto started = std::chrono::steady_clock::now();
    bool produced = false;

    while (index < history.size() && history[index].t <= position)
    {
        const RawMessage& message = history[index];
        ++index;

        VideoDecoder::AccessUnit unit;
        unit.data = std::span<const std::uint8_t>(message.payload.data(), message.payload.size());
        unit.codec = ((message.flags & kFlagH265) != 0) ? VideoDecoder::Codec::H265
                                                        : VideoDecoder::Codec::H264;
        unit.is_config = (message.flags & RawMessage::kPreamble) != 0;
        unit.is_keyframe = (message.flags & RawMessage::kSeekPoint) != 0 && !unit.is_config;
        unit.t = message.t;

        produced = decoder_.submit(unit) || produced;
        decoded_through_ = message.t;

        // Budgeted, and checked AFTER at least one access unit so a slow decoder
        // still makes progress rather than spinning without ever advancing.
        const double elapsed_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
                .count();
        if (elapsed_ms >= kDecodeBudgetMs)
        {
            break;
        }
    }

    return produced;
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

    const bool produced = decodeUpTo(position);

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

    const QImage& image = decoder_.image();
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
    if (cfg_.zenoh_key.empty())
    {
        message = tr("Drop a video topic here");
    }
    else if (bind_failed_)
    {
        message = tr("Could not bind %1").arg(QString::fromStdString(cfg_.zenoh_key));
    }
    else if (decoder_.stats().received == 0)
    {
        message = tr("Waiting for %1").arg(QString::fromStdString(cfg_.zenoh_key));
    }
    else if (!decoder_.synced())
    {
        message = tr("Waiting for a keyframe (%1 dropped)")
                      .arg(decoder_.stats().dropped_before_sync);
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
        decoder_.reset();
        decoder_.resetStats();
        seek_points_.clear();
        window_valid_ = false;
        bindStream();
    }
    else if (buffer_)
    {
        buffer_->setBounds(std::max(cfg_.retention_seconds, kMinRetentionSeconds),
                           static_cast<std::size_t>(cfg_.max_buffer_bytes));
    }

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

    const VideoDecoder::Stats& decoded = decoder_.stats();
    out.received = decoded.received;
    out.decoded = decoded.decoded;
    out.dropped_before_sync = decoded.dropped_before_sync;
    out.decode_errors = decoded.decode_errors;
    out.convert_errors = decoded.convert_errors;
    out.synced = decoder_.synced();

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

    const QImage& image = decoder_.image();
    if (!image.isNull())
    {
        out.has_frame = true;
        out.frame_t = decoder_.frameTime();
        out.frame_width = static_cast<std::uint64_t>(image.width());
        out.frame_height = static_cast<std::uint64_t>(image.height());
    }

    return out;
}

}  // namespace scope
