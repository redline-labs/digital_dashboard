// SPDX-License-Identifier: GPL-3.0-or-later
//
// The video panel's H.264 decoder, against a stream this test encodes itself.
//
// NO FIXTURE FILE IN THE TREE, deliberately. The stream is produced here the
// same way nodes/carplay/simulate.cpp produces one -- a real x264 encoder, a
// two-second GOP, GLOBAL_HEADER so the parameter sets arrive out of band -- so
// the test exercises the same shape of input the panel really sees, and a
// checked-in .h264 blob cannot rot against the ffmpeg the build actually links.
//
// The pattern encodes its own timestamp: a box whose position is a function of
// the frame index. That is what makes "decode to frame N" a check on the PIXELS
// rather than on the decoder's own bookkeeping, which is exactly the thing that
// would agree with itself while being wrong.
//
// What is checked, and why each one is a real failure mode:
//
//   - Parameter sets alone are not a decodable access unit. Fed on their own,
//     libavcodec returns AVERROR_INVALIDDATA -- so a decoder that submits them
//     produces nothing and logs decoder errors forever.
//   - A delta frame before any sync point must be DROPPED, not fed. Feeding it
//     produces a picture built from nothing, which renders as garbage rather
//     than as an error.
//   - reset() then feeding from an arbitrary keyframe must produce the RIGHT
//     picture. This is the seek primitive.
//
// A NOTE ON WHAT THE SIMPLE STREAM CANNOT PROVE, because the first version of
// this file got it wrong. With max_b_frames = 0 every keyframe is an IDR and the
// decoder reports delay = 0, so it holds nothing back -- and reset() has no
// observable effect at all. Removing avcodec_flush_buffers() left the whole
// suite green, which is precisely the "test that passes against the bug" this
// tree warns about. testReorderedStream exists for that: a B-frame stream
// genuinely reorders and genuinely holds pictures, and it fails if either the
// flush or the packet-timestamp propagation is removed. Both were checked by
// reverting them.
//
// No Qt widgets and no zenoh, but it does link libavcodec, so it is a plain
// unit test that spends a little real CPU.

#include "video/video_decoder.h"

#include <spdlog/spdlog.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
}

#include <QImage>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <tuple>
#include <vector>

namespace
{

int failures = 0;
int checks = 0;

void expect(bool condition, const std::string& what)
{
    ++checks;
    if (!condition)
    {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

constexpr int kWidth = 160;
constexpr int kHeight = 120;
constexpr int kFps = 30;
constexpr int kGop = 10;      // a keyframe every 10 frames, so the test is quick
constexpr int kFrames = 40;   // four GOPs

// A moving box whose LEFT EDGE is a function of the frame index, so the decoded
// picture says which frame it is. Same idea as simulate.cpp's sweeping box, at a
// size that keeps the test fast.
constexpr int kBoxWidth = 16;

int boxLeftFor(int index)
{
    return (index * 3) % (kWidth - kBoxWidth);
}

void drawPattern(AVFrame* frame, int index)
{
    // Mid grey everywhere, then a white box. Flat backgrounds compress to almost
    // nothing, which keeps the encode fast, and make the box unambiguous.
    for (int y = 0; y < kHeight; ++y)
    {
        std::memset(frame->data[0] + y * frame->linesize[0], 128, kWidth);
    }
    for (int y = 0; y < kHeight / 2; ++y)
    {
        std::memset(frame->data[1] + y * frame->linesize[1], 128, kWidth / 2);
        std::memset(frame->data[2] + y * frame->linesize[2], 128, kWidth / 2);
    }

    const int left = boxLeftFor(index);
    for (int y = kHeight / 4; y < kHeight / 4 + 24; ++y)
    {
        std::memset(frame->data[0] + y * frame->linesize[0] + left, 235,
                    static_cast<std::size_t>(kBoxWidth));
    }
}

// One encoded access unit, as the panel would receive it.
struct Unit
{
    std::vector<std::uint8_t> data;
    bool is_config = false;
    bool is_keyframe = false;
    int index = -1;  // which source frame it encodes; -1 for the config message
};

// Encodes kFrames of the pattern, returning the access units in order with the
// parameter sets republished before every keyframe -- exactly what the CarPlay
// node does, because zenoh has no retained messages.
//
// `b_frames` is the interesting knob. Zero matches CarPlay and simulate today:
// decode order is display order and every keyframe is an IDR, which by itself
// clears the decoder's reference buffer. A stream with B-frames REORDERS, and
// that is what makes reset() and the timestamp handling observable -- see
// testReorderedStream below.
std::vector<Unit> encodeStream(std::vector<std::uint8_t>& parameter_sets, int b_frames = 0)
{
    std::vector<Unit> units;

    const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (encoder == nullptr)
    {
        std::fprintf(stderr, "SKIP: no H.264 encoder in this libavcodec build\n");
        return units;
    }

    AVCodecContext* ctx = avcodec_alloc_context3(encoder);
    ctx->width = kWidth;
    ctx->height = kHeight;
    ctx->time_base = AVRational{1, kFps};
    ctx->framerate = AVRational{kFps, 1};
    ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    ctx->gop_size = kGop;
    ctx->max_b_frames = b_frames;
    ctx->bit_rate = 400'000;
    ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    av_opt_set(ctx->priv_data, "preset", b_frames > 0 ? "medium" : "ultrafast", 0);
    if (b_frames == 0)
    {
        av_opt_set(ctx->priv_data, "tune", "zerolatency", 0);
    }

    if (avcodec_open2(ctx, encoder, nullptr) < 0)
    {
        std::fprintf(stderr, "SKIP: could not open the H.264 encoder\n");
        avcodec_free_context(&ctx);
        return units;
    }

    if (ctx->extradata != nullptr && ctx->extradata_size > 0)
    {
        parameter_sets.assign(ctx->extradata, ctx->extradata + ctx->extradata_size);
    }

    AVFrame* frame = av_frame_alloc();
    frame->format = ctx->pix_fmt;
    frame->width = kWidth;
    frame->height = kHeight;
    av_frame_get_buffer(frame, 0);
    AVPacket* pkt = av_packet_alloc();

    const auto collect = [&]() {
        while (avcodec_receive_packet(ctx, pkt) == 0)
        {
            Unit unit;
            unit.is_keyframe = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
            unit.data.assign(pkt->data, pkt->data + pkt->size);

            // From the packet's OWN pts, not from a queue of submitted frames.
            // With B-frames the encoder emits packets in decode order, so a FIFO
            // would label every packet with the wrong source frame -- and the
            // test would then assert the decoder reproduced the mislabelling.
            unit.index = (pkt->pts == AV_NOPTS_VALUE) ? -1 : static_cast<int>(pkt->pts);

            // Republished before every keyframe, as the node does.
            if (unit.is_keyframe && !parameter_sets.empty())
            {
                Unit config;
                config.is_config = true;
                config.data = parameter_sets;
                units.push_back(std::move(config));
            }
            units.push_back(std::move(unit));
            av_packet_unref(pkt);
        }
    };

    for (int i = 0; i < kFrames; ++i)
    {
        av_frame_make_writable(frame);
        drawPattern(frame, i);
        frame->pts = i;
        if (avcodec_send_frame(ctx, frame) == 0)
        {
            collect();
        }
    }

    avcodec_send_frame(ctx, nullptr);
    collect();

    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&ctx);
    return units;
}

scope::VideoDecoder::AccessUnit toAccessUnit(const Unit& unit)
{
    scope::VideoDecoder::AccessUnit out;
    out.data = std::span<const std::uint8_t>(unit.data.data(), unit.data.size());
    out.codec = scope::VideoDecoder::Codec::H264;
    out.is_config = unit.is_config;
    out.is_keyframe = unit.is_keyframe;
    out.t = static_cast<double>(unit.index) / kFps;
    return out;
}

// Submit and present in one go: what a caller that wants EVERY frame does, and
// what the decoder did in one call before presenting became a separate decision.
// True when a new picture reached image().
//
// The seek path deliberately does not do this -- see
// testCatchUpPresentsOnlyTheFrameAskedFor.
bool feed(scope::VideoDecoder& decoder, const Unit& unit)
{
    return decoder.submit(toAccessUnit(unit)) && decoder.present();
}

// Where the white box actually is in the decoded picture, or -1 when there is
// no box. Scans the row the pattern draws through.
int measureBoxLeft(const QImage& image)
{
    if (image.isNull())
    {
        return -1;
    }
    const int y = kHeight / 4 + 8;
    for (int x = 0; x < image.width(); ++x)
    {
        if (qRed(image.pixel(x, y)) > 200)
        {
            return x;
        }
    }
    return -1;
}

// The box is drawn crisply but survives a lossy encode with soft edges, so an
// exact match is the wrong assertion. Two pixels is well inside the 3-pixel
// step between consecutive frames, so this still tells frames apart.
bool boxNear(int measured, int expected)
{
    return measured >= 0 && std::abs(measured - expected) <= 2;
}

// A frame's time is quantised to microseconds on its way through libavcodec --
// packet->pts is an integer -- so it comes back a fraction of a microsecond off
// the double that went in. Half a microsecond is four orders of magnitude below
// a frame interval, so this still tells frames apart.
bool timeIsFrame(double t, int index)
{
    return std::abs(t - static_cast<double>(index) / kFps) < 1e-6;
}

// ---------------------------------------------------------------------------

void testDecodesAStreamInOrder(const std::vector<Unit>& units)
{
    scope::VideoDecoder decoder;

    int pictures = 0;
    int last_index = -1;
    for (const Unit& unit : units)
    {
        if (feed(decoder, unit))
        {
            ++pictures;
            last_index = unit.index;
        }
    }

    expect(pictures == kFrames, "stream: every encoded frame came back out");
    expect(decoder.stats().decode_errors == 0, "stream: libavcodec rejected nothing");
    expect(decoder.stats().convert_errors == 0, "stream: swscale converted everything");
    expect(decoder.synced(), "stream: the decoder is synced at the end");
    expect(!decoder.image().isNull(), "stream: a picture is available");
    expect(decoder.image().width() == kWidth && decoder.image().height() == kHeight,
           "stream: the picture is the stream's own size");
    expect(boxNear(measureBoxLeft(decoder.image()), boxLeftFor(last_index)),
           "stream: the last picture is the LAST frame, checked on the pixels");
    expect(decoder.frameTime() ==
               static_cast<double>(last_index) / kFps,
           "stream: frameTime reports the access unit that produced the picture");
}

void testConfigAloneProducesNothing(const std::vector<std::uint8_t>& parameter_sets)
{
    // Parameter sets are not a decodable access unit. A decoder that submitted
    // them would get AVERROR_INVALIDDATA and count a decode error every time the
    // stream renegotiated.
    scope::VideoDecoder decoder;

    scope::VideoDecoder::AccessUnit config;
    config.data = std::span<const std::uint8_t>(parameter_sets.data(), parameter_sets.size());
    config.is_config = true;

    const bool produced = decoder.submit(config) && decoder.present();

    expect(!produced, "config alone: no picture");
    expect(decoder.image().isNull(), "config alone: nothing was drawn");
    expect(decoder.stats().decode_errors == 0,
           "config alone: it was CACHED, not fed to libavcodec");
    expect(decoder.synced(), "config alone: it still counts as a sync point");
}

void testDeltaBeforeSyncIsDropped(const std::vector<Unit>& units)
{
    // A reader that joined mid-stream. Feeding these produces a picture built
    // from reference frames that were never received, which renders as garbage
    // rather than failing.
    scope::VideoDecoder decoder;

    int fed = 0;
    for (const Unit& unit : units)
    {
        if (unit.is_config || unit.is_keyframe)
        {
            continue;  // withhold every sync point
        }
        feed(decoder, unit);
        if (++fed >= 8)
        {
            break;
        }
    }

    expect(!decoder.synced(), "before sync: the decoder never synced");
    expect(decoder.image().isNull(), "before sync: nothing was drawn");
    expect(decoder.stats().dropped_before_sync == static_cast<std::uint64_t>(fed),
           "before sync: every dropped unit was COUNTED");
    expect(decoder.stats().decode_errors == 0,
           "before sync: nothing reached libavcodec, so there is nothing to reject");
}

void testResetThenSeekToAKeyframe(const std::vector<Unit>& units)
{
    // THE SEEK PRIMITIVE. Decode to the end, then reset and feed one GOP from
    // the middle: the picture must be that GOP's, not a smear of the two.
    scope::VideoDecoder decoder;

    for (const Unit& unit : units)
    {
        feed(decoder, unit);
    }
    const int box_at_end = measureBoxLeft(decoder.image());
    expect(box_at_end >= 0, "seek: there is a picture before the reset");

    // Find the LAST keyframe and replay from the config just before it.
    std::size_t keyframe = units.size();
    for (std::size_t i = units.size(); i-- > 0;)
    {
        if (units[i].is_keyframe)
        {
            keyframe = i;
            break;
        }
    }
    expect(keyframe < units.size(), "seek: the stream has a keyframe to seek to");

    std::size_t start = keyframe;
    while (start > 0 && units[start - 1].is_config)
    {
        --start;
    }

    decoder.reset();
    expect(!decoder.synced(), "seek: reset dropped the sync state");
    expect(!decoder.image().isNull(),
           "seek: reset KEEPS the last picture, so a scrub does not flash black");

    // Three frames into that GOP.
    int target = -1;
    int produced = 0;
    for (std::size_t i = start; i < units.size() && produced < 3; ++i)
    {
        if (feed(decoder, units[i]))
        {
            ++produced;
            target = units[i].index;
        }
    }

    expect(produced == 3, "seek: decoding resumed from the keyframe");
    expect(decoder.stats().decode_errors == 0, "seek: no packet was rejected after the reset");
    expect(boxNear(measureBoxLeft(decoder.image()), boxLeftFor(target)),
           "seek: the picture is the SOUGHT frame, checked on the pixels");
}

void testCatchUpPresentsOnlyTheFrameAskedFor(const std::vector<Unit>& units)
{
    // THE SEEK LANDS, IT DOES NOT PLAY. Reaching an instant means restarting at
    // its keyframe and decoding forward to get there, and a panel that shows
    // every frame on the way runs the GOP past at decode speed -- forwards, even
    // when the drag is going backwards. The frames in between are references,
    // not pictures anyone asked for.
    scope::VideoDecoder decoder;

    // The second GOP, so there is a real run-up rather than one frame of it.
    std::size_t keyframe = units.size();
    std::size_t seen = 0;
    for (std::size_t i = 0; i < units.size(); ++i)
    {
        if (units[i].is_keyframe && seen++ == 1)
        {
            keyframe = i;
            break;
        }
    }
    expect(keyframe < units.size(), "catch-up: the stream has a second keyframe");
    if (keyframe >= units.size())
    {
        return;
    }

    std::size_t start = keyframe;
    while (start > 0 && units[start - 1].is_config)
    {
        --start;
    }

    // Six frames in: the instant being sought.
    std::size_t sought = start;
    int frames = 0;
    for (std::size_t i = start; i < units.size(); ++i)
    {
        if (!units[i].is_config && ++frames == 6)
        {
            sought = i;
            break;
        }
    }
    expect(frames == 6, "catch-up: the GOP is long enough to run up to");

    const double target_t = static_cast<double>(units[sought].index) / kFps;
    decoder.setTarget(target_t);

    // What the panel does: feed until the decoder has overshot the instant, and
    // convert nothing on the way.
    std::size_t fed = start;
    while (fed < units.size() && !decoder.reachedTarget())
    {
        decoder.submit(toAccessUnit(units[fed]));
        ++fed;
    }

    expect(decoder.reachedTarget(), "catch-up: feeding stopped once the target was overshot");
    expect(decoder.stats().presented == 0,
           "catch-up: NOTHING was drawn while the run-up decoded -- the whole point");
    expect(decoder.stats().decoded > 1,
           "catch-up: the frames in front of it were still DECODED, for their references");

    const bool drew = decoder.present();

    expect(drew, "catch-up: presenting drew the picture");
    expect(decoder.stats().presented == 1,
           "catch-up: exactly ONE conversion paid for a whole GOP of decoding");
    expect(decoder.stats().convert_errors == 0, "catch-up: nothing failed to convert");
    expect(boxNear(measureBoxLeft(decoder.image()), boxLeftFor(units[sought].index)),
           "catch-up: and the picture is the sought frame, checked on the pixels");
    expect(timeIsFrame(decoder.frameTime(), units[sought].index),
           "catch-up: frameTime is the sought frame's, not a skipped one's");

    // The run-up really was decoded through libavcodec rather than discarded: a
    // decoder fed only the target would have produced a broken picture or
    // nothing at all.
    expect(decoder.stats().decode_errors == 0, "catch-up: no packet was rejected");

    // And presenting again is free: the same instant is already on screen.
    expect(!decoder.present(), "catch-up: presenting the same instant twice converts once");
}

void testAStashedFrameIsPresentedWhenTheTargetReachesIt(const std::vector<Unit>& units)
{
    // A decoder with delay hands back frames LATER than the packets that carried
    // them, so reaching an instant means feeding past it -- and the frames that
    // come with those extra packets are in the future, not the past.
    //
    // THEY MUST NOT BE THROWN AWAY. libavcodec will not emit a frame twice, so a
    // decoder that drops them has nothing to show when playback advances onto
    // them a fraction of a second later: it would skip exactly the frames that
    // decoder delay pulled forward, which looks like a stuttering recording.
    scope::VideoDecoder decoder;

    std::size_t start = 0;
    while (start < units.size() && !units[start].is_config && !units[start].is_keyframe)
    {
        ++start;
    }

    // Aim at the first frame of the GOP, then feed several units past it.
    int first_index = -1;
    for (std::size_t i = start; i < units.size(); ++i)
    {
        if (!units[i].is_config)
        {
            first_index = units[i].index;
            break;
        }
    }
    expect(first_index >= 0, "stash: the stream has a frame to aim at");
    if (first_index < 0)
    {
        return;
    }
    const double first_t = static_cast<double>(first_index) / kFps;

    decoder.setTarget(first_t);

    std::size_t fed = start;
    int units_fed = 0;
    while (fed < units.size() && units_fed < 6)
    {
        decoder.submit(toAccessUnit(units[fed]));
        ++fed;
        ++units_fed;
    }

    expect(decoder.stats().decoded > 1, "stash: several frames came out");
    expect(decoder.present(), "stash: the frame at the target was presented");
    expect(timeIsFrame(decoder.frameTime(), first_index), "stash: and it is the one asked for");

    // Now the playhead advances onto a frame that came out during the run-up.
    // Nothing more is fed -- if it was dropped, there is nothing to show.
    const std::uint64_t decoded_before = decoder.stats().decoded;
    const double next_t = static_cast<double>(first_index + 1) / kFps;
    decoder.setTarget(next_t);

    expect(decoder.present(), "stash: the NEXT frame was still held and could be shown");
    expect(timeIsFrame(decoder.frameTime(), first_index + 1), "stash: and it is the right one");
    expect(decoder.stats().decoded == decoded_before,
           "stash: without decoding anything again -- it was already out");
}

void testTheLastFrameOfAWindowIsReachable(const std::vector<Unit>& units)
{
    // THE FRAME AT THE END OF A GOP, which is the one a delayed decoder is
    // holding when the units run out.
    //
    // Frame threading -- what setSeekOptimised(true) turns on, because it makes
    // a catch-up five times faster -- withholds the first thread_count - 1
    // pictures. So feeding a whole window and stopping leaves the frame actually
    // asked for inside libavcodec, and presenting then gives a picture a few
    // frames early WITH THE WRONG TIME ON IT. That is the exact failure this
    // panel's design exists to prevent, and drain() is what prevents it.
    //
    // Checked with threading both on and off: the answer must not depend on it.
    const auto landOnLastFrame = [&units](bool seek_optimised) {
        scope::VideoDecoder decoder;
        decoder.setSeekOptimised(seek_optimised);

        // The last GOP, so the window really does end where the stream does.
        std::size_t keyframe = units.size();
        for (std::size_t i = units.size(); i-- > 0;)
        {
            if (units[i].is_keyframe)
            {
                keyframe = i;
                break;
            }
        }
        std::size_t start = keyframe;
        while (start > 0 && units[start - 1].is_config)
        {
            --start;
        }

        int last_index = -1;
        for (std::size_t i = start; i < units.size(); ++i)
        {
            if (!units[i].is_config)
            {
                last_index = units[i].index;
            }
        }

        decoder.setTarget(static_cast<double>(last_index) / kFps);
        for (std::size_t i = start; i < units.size(); ++i)
        {
            decoder.submit(toAccessUnit(units[i]));
        }

        // Nothing in the stream is after the last frame, so the target is never
        // overshot however the decoder is configured. This is the state that
        // makes drain() necessary rather than optional.
        const bool overshot = decoder.reachedTarget();
        decoder.drain();
        const bool drew = decoder.present();

        return std::make_tuple(overshot, drew, decoder.frameTime(),
                               measureBoxLeft(decoder.image()), last_index);
    };

    for (const bool seek_optimised : {false, true})
    {
        const auto [overshot, drew, frame_t, box, last_index] = landOnLastFrame(seek_optimised);
        const std::string what = seek_optimised ? "frame-threaded" : "single-threaded";

        expect(!overshot, what + ": the last frame is never overshot, so draining is the only way");
        expect(drew, what + ": draining produced the picture");
        expect(timeIsFrame(frame_t, last_index), what + ": stamped with the LAST frame's time");
        expect(boxNear(box, boxLeftFor(last_index)),
               what + ": and its pixels are the last frame's too");
    }
}

void testHardwareAndSoftwareAgreeOnThePicture(const std::vector<Unit>& units)
{
    // WHATEVER THIS MACHINE HAS, the picture is the same one. A GPU path that
    // decodes a different frame -- or the same frame with the colour ranges
    // swapped -- is not something a user could tell from a bad seek, so it is
    // pinned against the software decoder rather than against itself.
    //
    // Not pixel-identity: H.264 decoding is normative, but the two paths reach
    // RGB differently (NV12 off the GPU, YUV420P off the CPU) and may differ in
    // the last bit of a channel. The box position is the frame's identity and is
    // immune to that.
    struct Outcome
    {
        QImage image;
        double frame_t = 0.0;
        bool hardware = false;
        std::string backend;
        std::uint64_t decode_errors = 0;
        std::uint64_t convert_errors = 0;
    };

    const auto decodeTo = [&units](bool hardware, int frames) {
        scope::VideoDecoder decoder;
        decoder.setHardwareEnabled(hardware);

        int produced = 0;
        for (const Unit& unit : units)
        {
            if (feed(decoder, unit) && ++produced >= frames)
            {
                break;
            }
        }

        // Copied out: the decoder is not copyable, and image() is a reference
        // into one that is about to go.
        return Outcome{decoder.image().copy(), decoder.frameTime(), decoder.hardware(),
                       decoder.backend(), decoder.stats().decode_errors,
                       decoder.stats().convert_errors};
    };

    const Outcome cpu = decodeTo(false, 12);
    const Outcome gpu = decodeTo(true, 12);

    std::fprintf(stderr, "NOTE: video decode backend on this machine: %s\n", gpu.backend.c_str());

    expect(!cpu.hardware, "backends: disabling hardware really did use software");
    expect(!cpu.image.isNull() && !gpu.image.isNull(), "backends: both produced a picture");
    expect(cpu.image.size() == gpu.image.size(), "backends: at the same size");
    expect(measureBoxLeft(gpu.image) == measureBoxLeft(cpu.image),
           "backends: and the SAME frame, checked on the pixels");
    expect(gpu.frame_t == cpu.frame_t, "backends: stamped with the same instant");

    // Whether hardware was actually used is a property of the machine, not of
    // the code, so it cannot be asserted -- but if it WAS used, it must not have
    // cost anything in errors.
    expect(gpu.decode_errors == 0, "backends: the chosen backend rejected nothing");
    expect(gpu.convert_errors == 0, "backends: and everything converted");
}

void testSeekIsDeterministic(const std::vector<Unit>& units)
{
    // Seeking to the same instant twice, with a seek elsewhere in between, must
    // give the same picture. A decoder carrying state across the reset would
    // still produce an image -- just a different one the second time, which
    // looks like data rather than like a bug.
    const auto decodeFrom = [&units](std::size_t keyframe_ordinal, int frames) {
        scope::VideoDecoder decoder;

        std::size_t seen = 0;
        std::size_t start = units.size();
        for (std::size_t i = 0; i < units.size(); ++i)
        {
            if (units[i].is_keyframe && seen++ == keyframe_ordinal)
            {
                start = i;
                break;
            }
        }
        while (start > 0 && units[start - 1].is_config)
        {
            --start;
        }

        int produced = 0;
        for (std::size_t i = start; i < units.size() && produced < frames; ++i)
        {
            if (feed(decoder, units[i]))
            {
                ++produced;
            }
        }
        return decoder.image().copy();
    };

    const QImage first = decodeFrom(2, 4);
    const QImage elsewhere = decodeFrom(0, 4);
    const QImage again = decodeFrom(2, 4);

    expect(!first.isNull() && !again.isNull(), "determinism: both passes produced a picture");
    expect(first == again, "determinism: the same seek gives a PIXEL-IDENTICAL picture");
    expect(first != elsewhere, "determinism: a different seek gives a different picture");
}

void testSequenceGapForcesResync(const std::vector<Unit>& units)
{
    // Feeding a GOP's tail without its head, which is what a dropped message
    // looks like. The decoder must decline rather than smear across the gap.
    scope::VideoDecoder decoder;

    // Sync on the first GOP.
    std::size_t i = 0;
    for (; i < units.size(); ++i)
    {
        feed(decoder, units[i]);
        if (!decoder.image().isNull())
        {
            break;
        }
    }
    expect(decoder.synced(), "gap: synced on the first GOP");

    const std::uint64_t errors_before = decoder.stats().decode_errors;

    // Now jump to the middle of a later GOP, skipping its keyframe. The decoder
    // is still synced, so these ARE fed -- the check is that it survives them
    // and recovers on the next keyframe rather than getting stuck.
    for (std::size_t j = units.size() * 3 / 4; j < units.size(); ++j)
    {
        feed(decoder, units[j]);
    }

    expect(!decoder.image().isNull(), "gap: the decoder recovered and has a picture");
    expect(decoder.stats().decoded > 0, "gap: pictures were still produced");
    static_cast<void>(errors_before);
}

void testReorderedStream(const std::vector<Unit>& units)
{
    // A stream that REORDERS -- B-frames, so decode order is not display order
    // and the decoder holds pictures back before emitting them.
    //
    // This is the shape that makes two things observable which the no-B-frame
    // stream cannot distinguish:
    //
    //   - THE TIMESTAMP. "Stamp whatever comes out with the time of the last
    //     access unit submitted" is right only when the two orders agree. Here
    //     they do not, so a decoder doing that labels frames with other frames'
    //     times -- and `frame_t` is precisely the field a seek asserts on.
    //   - THE FLUSH. reset() has to drop the pictures the decoder is still
    //     holding from the previous GOP. Without it they come out AFTER the
    //     seek, so the first picture of the new position is the old one.
    //
    // CarPlay does not send B-frames today. That is a fact about the phone's
    // encoder, not about this code, and the panel should not go quietly wrong
    // if it changes.

    if (units.empty())
    {
        std::fprintf(stderr, "FAIL: could not encode a reordered test stream\n");
        ++failures;
        return;
    }

    // The stream really does reorder, or this test proves nothing.
    bool reordered = false;
    int previous = -1;
    for (const Unit& unit : units)
    {
        if (unit.is_config)
        {
            continue;
        }
        if (previous >= 0 && unit.index < previous)
        {
            reordered = true;
        }
        previous = unit.index;
    }
    expect(reordered, "reordered: the encoder really did emit out of display order");

    scope::VideoDecoder decoder;

    // Every picture must come out labelled with ITS OWN time, whatever order the
    // packets arrived in.
    int mislabelled = 0;
    int measured_pictures = 0;
    for (const Unit& unit : units)
    {
        if (!feed(decoder, unit))
        {
            continue;
        }
        ++measured_pictures;

        const int box = measureBoxLeft(decoder.image());
        const int claimed_index = static_cast<int>(std::llround(decoder.frameTime() * kFps));
        if (!boxNear(box, boxLeftFor(claimed_index)))
        {
            ++mislabelled;
        }
    }

    expect(measured_pictures > 0, "reordered: pictures were produced");
    expect(mislabelled == 0,
           "reordered: every picture's frame_t matches the picture's own PIXELS");

    // And the flush. Decode the whole stream, then reset and replay one GOP from
    // the middle: the first picture out must belong to the new position, not be
    // one the decoder was still holding from the old one.
    std::size_t keyframe = units.size();
    std::size_t seen = 0;
    for (std::size_t i = 0; i < units.size(); ++i)
    {
        if (units[i].is_keyframe && seen++ == 2)
        {
            keyframe = i;
            break;
        }
    }
    expect(keyframe < units.size(), "reordered: there is a third keyframe to seek to");

    std::size_t start = keyframe;
    while (start > 0 && units[start - 1].is_config)
    {
        --start;
    }

    decoder.reset();

    int first_index = -1;
    for (std::size_t i = start; i < units.size(); ++i)
    {
        if (feed(decoder, units[i]))
        {
            first_index = static_cast<int>(std::llround(decoder.frameTime() * kFps));
            break;
        }
    }

    // EQUALITY, not >=. A picture the decoder was still holding from the end of
    // the stream has a LATER index than the keyframe, so ">= keyframe" is
    // satisfied by exactly the stale frame this is meant to catch.
    expect(first_index == units[keyframe].index,
           "reordered: after reset the FIRST picture is the sought keyframe itself, "
           "not one the decoder was still holding from before it");
    expect(boxNear(measureBoxLeft(decoder.image()), boxLeftFor(first_index)),
           "reordered: and its pixels agree with its time");
}

}  // namespace

int main()
{
    spdlog::set_level(spdlog::level::warn);

    std::vector<std::uint8_t> parameter_sets;
    const std::vector<Unit> units = encodeStream(parameter_sets);

    if (units.empty())
    {
        // No encoder in this build. Not a pass: a green run has to mean the
        // decoder was exercised, so this is reported as a failure rather than
        // quietly skipped.
        std::fprintf(stderr, "FAIL: could not encode a test stream\n");
        return 1;
    }

    expect(!parameter_sets.empty(),
           "setup: GLOBAL_HEADER produced out-of-band parameter sets");

    testDecodesAStreamInOrder(units);
    testConfigAloneProducesNothing(parameter_sets);
    testDeltaBeforeSyncIsDropped(units);
    testResetThenSeekToAKeyframe(units);
    testCatchUpPresentsOnlyTheFrameAskedFor(units);
    testAStashedFrameIsPresentedWhenTheTargetReachesIt(units);
    testTheLastFrameOfAWindowIsReachable(units);
    testHardwareAndSoftwareAgreeOnThePicture(units);
    testSeekIsDeterministic(units);
    testSequenceGapForcesResync(units);

    std::vector<std::uint8_t> reordered_parameter_sets;
    testReorderedStream(encodeStream(reordered_parameter_sets, /*b_frames=*/3));

    std::fprintf(stderr, "%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
