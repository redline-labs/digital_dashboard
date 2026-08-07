#ifndef SCOPE_VIDEO_DECODER_H_
#define SCOPE_VIDEO_DECODER_H_

#include <QImage>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

// Forward declarations for libavcodec/libswscale, exactly as the CarPlay widget
// does, so nothing that merely holds a decoder pulls ffmpeg's headers in.
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

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
// SINGLE-THREADED BY CONSTRUCTION. Everything here runs on whichever thread
// calls it, holds no locks and owns no timer; the panel drives it from the
// shared render tick. thread_count is left at 1 for the reason measured in
// carplay_widget.cpp:136-151 -- frame threading withholds thread_count - 1
// pictures before emitting the first, which for a seek means the frame you
// asked for never arrives at all.
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

    // Feed one. True when a NEW picture landed in image().
    //
    // False is the ordinary case as often as not: a config message, an access
    // unit before sync, or a frame the decoder consumed without emitting.
    // Nothing about it is an error, which is why the counters below are how you
    // find out whether anything is actually wrong.
    bool submit(const AccessUnit& unit);

    // Throw away all decoder state and go back to waiting for a sync point.
    //
    // THE SEEK PRIMITIVE. Without it a scrub backwards feeds frames from before
    // what the decoder has already consumed, and libavcodec either rejects them
    // or -- worse -- emits a picture built from two unrelated stretches of the
    // stream, which looks like a corrupt frame rather than a bug.
    //
    // Keeps the last decoded image, so a scrub does not flash black between
    // releasing the old GOP and decoding the new one.
    void reset();

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

    // Everything a caller needs to tell "nothing has arrived" from "everything
    // arrived and none of it could be decoded", which look identical on screen.
    struct Stats
    {
        std::uint64_t received = 0;
        std::uint64_t decoded = 0;
        std::uint64_t dropped_before_sync = 0;
        std::uint64_t decode_errors = 0;
        std::uint64_t convert_errors = 0;
    };
    const Stats& stats() const { return stats_; }
    void resetStats() { stats_ = Stats{}; }

  private:
    bool ensureDecoder(Codec codec);
    void destroyDecoder();

    // Converts and publishes into image_. False for anything swscale cannot
    // handle, which would otherwise render as black.
    bool renderFrame(const AVFrame* frame);

    AVCodecContext* context_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* packet_ = nullptr;
    Codec codec_ = Codec::H264;

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
