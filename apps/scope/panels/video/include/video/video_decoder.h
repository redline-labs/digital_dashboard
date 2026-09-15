#ifndef SCOPE_VIDEO_DECODER_H_
#define SCOPE_VIDEO_DECODER_H_

#include <QImage>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <vector>

// Forward declarations for libavcodec/libswscale, exactly as the CarPlay widget
// does, so nothing that merely holds a decoder pulls ffmpeg's headers in.
struct AVBufferRef;
struct AVCodec;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

// The ONE exception, and only because a C enum cannot be forward declared: the
// hardware path's get_format callback is typed in terms of AVPixelFormat. This
// is a header of enums and macros -- it pulls in no library, and nothing else
// here needs it.
extern "C" {
#include <libavutil/pixfmt.h>
}

namespace scope
{

// H.264/H.265 Annex-B access units in, a QImage out.
//
// A SECOND DECODER, deliberately, and not a refactor of CarPlayWidget's. That
// one is load-bearing on a screen someone is touching, and its decode settings
// are the result of measurement against a real capture -- pulling it out from
// under a working CarPlay path to serve a review tool is a bad trade. What is
// copied here is the *knowledge*, and the four things below are all of it:
//
//   - Parameter sets alone are NOT a decodable access unit. Fed to libavcodec on
//     their own they return AVERROR_INVALIDDATA. They have to be cached and
//     prepended to the next real access unit; Annex-B concatenates freely.
//   - A keyframe is a valid entry point even without a config message, because
//     Annex-B access units carry SPS/PPS in band. Gating on config alone leaves
//     a late-joining reader black until the stream happens to renegotiate.
//   - libavcodec reads up to AV_INPUT_BUFFER_PADDING_SIZE bytes past the end of
//     a packet it does not own, so the assembly buffer must carry that many
//     zeroed trailing bytes.
//   - Full-range 4:2:0 is tagged two different ways depending on decoder and
//     version (YUVJ420P, or YUV420P with color_range JPEG), and it is the range
//     that selects the conversion coefficients. Getting it wrong is not an
//     error, it is washed-out colour.
//
// And ONE thing CarPlayWidget does not have, because a live stream never needs
// it: reset(). A seek is a discontinuity, and feeding a new GOP to a decoder
// still holding the previous one smears the two together.
//
// NOT THREAD-SAFE, and owns no thread of its own. Everything here runs on
// whichever thread calls it, holds no locks and owns no timer -- see
// video_decode_worker.h, which is the thread. libavcodec's own decode threads
// are a separate matter, chosen by setSeekOptimised().
class VideoDecoder
{
  public:
    // Matches CarPlayVideo::Codec without depending on the schema: this class
    // takes bytes, and which codec they are is the caller's business to read.
    enum class Codec
    {
        H264,
        H265,
    };

    VideoDecoder();
    ~VideoDecoder();

    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    // One access unit, as it came off the wire.
    struct AccessUnit
    {
        std::span<const std::uint8_t> data;
        Codec codec = Codec::H264;
        bool is_config = false;
        bool is_keyframe = false;

        // Time on the source's clock. Carried through to frameTime() so a
        // caller can tell which instant the picture it is holding belongs to --
        // which is the whole assertion a seek test makes.
        double t = 0.0;
    };

    // ---------------------------------------------------------- the target
    //
    // DECODING AND PRESENTING ARE SEPARATE, and that separation is the whole
    // seek strategy -- the same one mpv and VLC use.
    //
    // Reaching an instant in an inter-frame codec means starting at its keyframe
    // and decoding forward to get there; there is no way to decode frame N
    // without frame N-1. What a player does NOT do is show the frames on the
    // way. They are references, not pictures anyone asked for, and presenting
    // them runs the GOP past at decode speed -- forwards, even when the drag is
    // going backwards, because a catch-up always runs forwards from the
    // keyframe.
    //
    // So: submit() decodes and keeps the best CANDIDATE, present() converts
    // exactly one. A sixty-frame catch-up costs sixty decodes and one
    // YUV->RGB pass instead of sixty of each.

    // The instant being asked for, on the source's clock. The frame presented is
    // the one with the greatest time at or before it, whatever order frames come
    // out in. Defaults to +infinity, which means "present whatever arrives" --
    // the right thing for a live stream, which is only ever at its own end.
    void setTarget(double t);
    double target() const { return target_; }

    // Whether a frame strictly AFTER the target has come out, which is what
    // makes the current candidate final: nothing still inside the decoder can
    // beat it. This is the caller's signal to stop feeding.
    //
    // False at the end of a stream, where the target is simply never overshot.
    // That case is what drain() is for.
    bool reachedTarget() const { return reached_; }

    // Whether there is anything for present() to draw. False after a reset and
    // until the first frame of the new position comes out -- which for a
    // DELAYED decoder means the caller has fed the whole window and the frame it
    // wants is still inside libavcodec. That is the one case drain() is for.
    bool hasCandidate() const { return candidate_ != nullptr; }

    // Feed one. True when the frame that WOULD be presented changed -- NOT that
    // anything was drawn, which is present()'s business.
    //
    // False is the ordinary case as often as not: a config message, an access
    // unit before sync, a frame the decoder consumed without emitting, or one
    // that lost to a candidate already held. Nothing about it is an error, which
    // is why the counters below are how you find out whether anything is wrong.
    bool submit(const AccessUnit& unit);

    // Push out the pictures libavcodec is still holding.
    //
    // A decoder with delay -- frame threading, or a stream with B-frames --
    // withholds frames until it has enough packets to emit them in display
    // order, so the last frame of a window is inside the decoder rather than
    // out of it. Without this, seeking to the end of a GOP would present the
    // frame a few before it and quietly claim to be at the right instant.
    //
    // Leaves the decoder ready for more; nothing is un-synced.
    bool drain();

    // Convert the candidate into image(). True when image() changed.
    //
    // THE ONE EXPENSIVE CALL, and the only one that touches swscale or copies a
    // picture back from the GPU. Call it once, when the catch-up is done.
    bool present();

    // Throw away all decoder state and go back to waiting for a sync point.
    //
    // THE SEEK PRIMITIVE. Without it a scrub backwards feeds frames from before
    // what the decoder has already consumed, and libavcodec either rejects them
    // or -- worse -- emits a picture built from two unrelated stretches of the
    // stream, which looks like a corrupt frame rather than a bug.
    //
    // Keeps the last presented image, so a scrub does not flash black between
    // releasing the old GOP and decoding the new one.
    void reset();

    // Drop the picture as well. For a change of SOURCE, where the times either
    // side are on different epochs and a kept frame can collide with an instant
    // in the new one -- the same reason TimeBase::setSource carries no position
    // across.
    void clearImage();

    // The most recently decoded picture, or a null image before the first.
    // Format_RGB32 at the stream's own resolution; the panel scales it when it
    // blits, because a scope panel is resized far more often than a dashboard
    // widget and rebuilding the scaler on every drag frame is worse than one
    // transform at paint time.
    const QImage& image() const { return image_; }

    // Source-clock time of the access unit that produced image(). Meaningless
    // until the first frame -- check image().isNull() first.
    double frameTime() const { return frame_t_; }

    bool synced() const { return synced_; }

    // --------------------------------------------------------- what it is for
    //
    // Scrubbing a recording and following a live bus want opposite decoders, so
    // this says which one is being built.
    //
    // ON: frame threading. Five times faster in a burst -- measured at 0.222 vs
    // 1.087 ms/frame on 1280x720, so a sixty-frame GOP catch-up is 13 ms rather
    // than 65 -- at the cost of withholding the first thread_count - 1 pictures.
    //
    // OFF: one thread, no delay. A live panel cannot pay that cost: four
    // withheld frames is 133 ms, and a picture that far behind the traces
    // drawn beside it is this panel failing at the one thing it is for.
    //
    // Changing it CLOSES the decoder; threading cannot be set under an open
    // context. The caller feeds from a keyframe again afterwards.
    void setSeekOptimised(bool on);
    bool seekOptimised() const { return seek_optimised_; }

    // ------------------------------------------------------------- hardware
    //
    // VideoToolbox on macOS, VAAPI on Linux, through libavcodec's generic
    // hw_device_ctx path -- so this is one code path with a per-platform
    // preference list rather than a decoder per API.
    //
    // OFF BY DEFAULT, AND THE MEASUREMENT IS WHY. The obvious expectation is
    // that the GPU wins the catch-up; on a 1280x720 stream it loses badly:
    //
    //     videotoolbox     1.673 ms/frame   -> 100 ms per GOP catch-up
    //     software, 1 thr  1.087 ms/frame   ->  65 ms
    //     software, frame  0.222 ms/frame   ->  13 ms
    //
    // A hardware decoder is built for real-time playback, and every frame pays
    // a session round-trip that a burst of sixty cannot amortise. What it is
    // genuinely good at is costing no CPU, and it may well win on a much larger
    // frame or a machine whose cores are slower -- which is why it stays, as a
    // measured choice rather than a default.
    //
    // The copy back from GPU memory is the one cost hardware adds beyond that,
    // and it is paid ONLY in present() -- so the frames of a catch-up, which are
    // never shown, never cross the bus at all.
    //
    // ALWAYS OPTIONAL. No device, a profile the GPU does not implement, or a
    // decoder that fails once on it: each falls back to software, which is
    // slower and correct rather than fast and black.
    //
    // Changing this CLOSES the decoder -- a backend cannot be swapped under an
    // open context -- so the caller must feed from a keyframe again afterwards.
    void setHardwareEnabled(bool on);
    bool hardwareEnabled() const { return hw_enabled_; }

    // Whether pictures are actually coming off the GPU right now, which is not
    // the same question as hardwareEnabled().
    bool hardware() const;

    // "videotoolbox", "vaapi", "software" -- for the stats panel and the log,
    // because "why is this seek slow" is otherwise unanswerable from outside.
    const char* backend() const { return backend_; }

    // Everything a caller needs to tell "nothing has arrived" from "everything
    // arrived and none of it could be decoded", which look identical on screen.
    struct Stats
    {
        std::uint64_t received = 0;
        std::uint64_t decoded = 0;

        // Pictures actually converted and shown. Far below `decoded` on a
        // recorded source and that is the design working: a catch-up decodes a
        // whole GOP to present its last frame. The two being EQUAL over a scrub
        // is the symptom of the fast-forward this is here to prevent.
        std::uint64_t presented = 0;

        std::uint64_t dropped_before_sync = 0;
        std::uint64_t decode_errors = 0;
        std::uint64_t convert_errors = 0;
    };
    const Stats& stats() const { return stats_; }
    void resetStats() { stats_ = Stats{}; }

  private:
    bool ensureDecoder(Codec codec);
    void destroyDecoder();

    // Open `decoder` on one device, or on none at all for software. Leaves
    // nothing behind when it fails, so the next attempt starts from clean.
    // `device_type` is an AVHWDeviceType, kept as an int to spare this header
    // libavutil/hwcontext.h.
    bool openWith(const AVCodec* decoder, int device_type);

    // libavcodec asking which pixel format to decode into. Picks the hardware
    // one when it is offered -- taking anything else is how a "hardware" decoder
    // quietly runs on the CPU.
    static AVPixelFormat chooseFormat(AVCodecContext* context, const AVPixelFormat* formats);

    // Give up on the GPU and reopen in software, for the rest of the session.
    // True when there is a working decoder afterwards.
    bool fallBackToSoftware();

    // Take everything libavcodec has ready and file it as candidate or future.
    // True when the candidate changed.
    bool collectFrames();

    // File one decoded frame against the target. Takes a REFERENCE to it, which
    // is a refcount rather than a picture copy -- frame_ is reused immediately.
    bool keepFrame(AVFrame* frame, double t);

    // Move anything in future_ that the target has now reached into the
    // candidate, and recompute reached_. True when the candidate changed.
    bool promoteFutureFrames();

    void clearHeldFrames();

    // Converts and publishes into image_. False for anything swscale cannot
    // handle, which would otherwise render as black.
    //
    // Takes a mutable frame because a HARDWARE one has to be copied back out of
    // video memory first, and av_hwframe_transfer_data reads through a
    // non-const source.
    bool renderFrame(AVFrame* frame);

    AVCodecContext* context_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* packet_ = nullptr;
    Codec codec_ = Codec::H264;

    // ------------------------------------------------------------- hardware

    // Off by default: on the numbers above, the GPU loses the catch-up.
    bool hw_enabled_ = false;

    // Frame threading, for a recording being scrubbed rather than a live bus.
    bool seek_optimised_ = false;

    // The GPU was tried and could not do it. Sticky for the session: retrying
    // per keyframe would re-pay the device setup to fail the same way.
    bool hw_failed_ = false;

    AVBufferRef* hw_device_ = nullptr;

    // The pixel format the GPU hands back -- a handle to a surface, not pixels.
    // AV_PIX_FMT_NONE while decoding in software, which is also the test
    // renderFrame() uses to decide whether a frame needs copying back.
    AVPixelFormat hw_pix_fmt_ = AV_PIX_FMT_NONE;

    // Where a hardware frame is copied to before conversion. Reused, so the
    // per-frame cost is the transfer rather than an allocation as well.
    AVFrame* sw_frame_ = nullptr;

    // Frames decoded since this context was opened, so a hardware decoder that
    // fails on its FIRST packet can be told apart from one that has been working
    // and hit a bad access unit. Only the former is worth falling back over.
    std::uint64_t decoded_on_context_ = 0;

    const char* backend_ = "software";

    // ------------------------------------------------------ what to present

    double target_ = std::numeric_limits<double>::infinity();

    // The best frame at or before the target so far, held as a reference. Not
    // converted until present() asks for it.
    AVFrame* candidate_ = nullptr;
    double candidate_t_ = 0.0;

    // Frames that came out AFTER the target, kept in case the target advances
    // onto them.
    //
    // WITHOUT THIS A DELAYED DECODER LOSES FRAMES. Playback feeds a few access
    // units past the playhead to flush out the one it wants; the frames that
    // come with them are a fraction of a second in the future, and libavcodec
    // will not emit them twice. Dropped, they are gone by the time the playhead
    // reaches them -- so playback would skip exactly the frames that decoder
    // delay pulled forward, which looks like a stuttering recording rather than
    // a bug here.
    //
    // Bounded, because a caller that never advances the target must not be able
    // to make this grow without limit. Held as references: a few refcounts, not
    // a few pictures.
    //
    // The time is carried alongside rather than re-read off the frame, because
    // deriving it a second time has to repeat the AV_NOPTS_VALUE fallback and
    // the fallback needs context that is gone by then.
    struct HeldFrame
    {
        AVFrame* frame = nullptr;
        double t = 0.0;
    };
    static constexpr std::size_t kMaxFutureFrames = 8;
    std::vector<HeldFrame> future_;

    // A frame strictly after the target has been seen, so the candidate is
    // final. See reachedTarget().
    bool reached_ = false;

    // Time of what is in image_, so present() can tell "already showing this"
    // from "nothing presented yet" -- which a bare frame_t_ of 0.0 cannot.
    bool has_presented_ = false;

    // Set once a sync point has been seen. Access units before that would only
    // produce decoder errors.
    bool synced_ = false;

    // Parameter sets waiting for the next access unit to be prepended to.
    std::vector<std::uint8_t> pending_config_;

    // Reusable Annex-B assembly buffer, so neither the packet nor the
    // config+frame concatenation allocates per frame. Always carries
    // AV_INPUT_BUFFER_PADDING_SIZE zeroed trailing bytes.
    std::vector<std::uint8_t> au_buffer_;

    SwsContext* sws_ = nullptr;
    int sws_width_ = 0;
    int sws_height_ = 0;
    int sws_src_format_ = -1;  // AVPixelFormat
    bool sws_full_range_ = false;

    QImage image_;
    double frame_t_ = 0.0;
    double pending_t_ = 0.0;

    Stats stats_;
};

}  // namespace scope

#endif  // SCOPE_VIDEO_DECODER_H_
