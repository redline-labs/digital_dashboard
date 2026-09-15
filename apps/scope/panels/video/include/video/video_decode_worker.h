#ifndef SCOPE_VIDEO_DECODE_WORKER_H_
#define SCOPE_VIDEO_DECODE_WORKER_H_

#include "video/video_decoder.h"

#include <QImage>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace scope
{

// The decoder, on a thread of its own.
//
// WHY THIS EXISTS. Reaching an instant in an inter-frame codec means decoding
// from its keyframe forward -- around sixty access units for a CarPlay GOP, at
// roughly a millisecond each in software. Done on the GUI thread that is a
// ~60 ms stall, so the panel used to ration it to 10 ms per render tick and a
// seek took seven ticks to land. That ration IS the "not instant" feeling: the
// work was never the problem, the fact that it was competing with painting was.
//
// Off the GUI thread there is no ration. A catch-up runs flat out and the window
// keeps repainting while it does, which is what every normal video player does
// and the only reason their seeks feel immediate.
//
// NO SIGNALS, NO QUEUED CONNECTIONS, NO MOC. The result is staged and collected
// on the panel's next render tick -- the same shape RecordedSource uses for its
// own worker, and for the same reason: the window already has one clock, and a
// second one delivering pictures between ticks would draw frames nobody asked
// for at whatever rate the decoder happened to finish at.
//
// LATEST REQUEST WINS. A drag makes one request per render tick and only the
// last is worth decoding, so a pending request replaces rather than queues --
// and a decode already running is abandoned mid-GOP once it is superseded. That
// is what keeps a fast scrub from falling further and further behind the pointer.
class VideoDecodeWorker
{
  public:
    // One access unit, owned. COPIED out of the panel's buffer rather than
    // referenced into it: that buffer is refilled from the GUI thread, and a
    // span into a vector another thread may reallocate is the sort of thing that
    // works until the recording is long enough.
    struct Unit
    {
        std::vector<std::uint8_t> data;
        double t = 0.0;
        bool is_config = false;
        bool is_keyframe = false;
        bool h265 = false;
    };

    // What the panel wants to see.
    struct Request
    {
        // Identifies the window these units belong to. A new id means the
        // decoder starts again -- a different GOP, or the same one refilled.
        std::uint64_t window_id = 0;

        // Replace the worker's window with `units`, or append to it. Appending
        // is what a live stream does as its current GOP grows: shipping the
        // whole window every tick would copy a megabyte thirty times a second
        // to add one frame to it.
        bool replace = true;

        std::vector<Unit> units;

        // The instant to present, on the source's clock.
        double position = 0.0;

        // Whether more units can still arrive for this window.
        //
        // A recording's window is one GOP, loaded whole: what is here is all
        // there will ever be, so a frame still inside a delayed decoder has to
        // be DRAINED out or the picture is silently a few frames early. A live
        // window grows every tick, so the same state means "wait a moment" --
        // and draining it would flush the decoder and re-decode the whole GOP,
        // thirty times a second, for ever.
        bool complete = true;

        // Decode for scrubbing rather than for following. Frame threading:
        // five times faster in a burst, at the price of holding the first few
        // pictures back. See VideoDecoder::setSeekOptimised().
        bool seek_optimised = false;
    };

    // A finished picture.
    struct Result
    {
        QImage image;
        double frame_t = 0.0;

        // The position this answers, so the panel can tell a picture for where
        // it is now from one for where it was two ticks ago.
        double position = 0.0;
    };

    // Everything the panel reports about decoding, copied out under the lock
    // because it lives on the other thread.
    struct Snapshot
    {
        VideoDecoder::Stats stats;
        bool synced = false;
        std::string backend = "software";
    };

    VideoDecodeWorker();
    ~VideoDecodeWorker();

    VideoDecodeWorker(const VideoDecodeWorker&) = delete;
    VideoDecodeWorker& operator=(const VideoDecodeWorker&) = delete;

    // Closes the decoder and reopens it on the other backend, then re-feeds the
    // window from its keyframe. Not free, and not meant to be: this is the knob
    // for "the GPU path is the suspect", so it has to actually take the GPU away
    // rather than wait for some later stream to apply to.
    void setHardwareEnabled(bool on);

    // Ask for a picture. Supersedes anything pending and abandons anything
    // running for an older request.
    void request(Request request);

    // Take the finished picture, if there is one. False when nothing new has
    // landed since the last call, which is the ordinary case on a tick where
    // nothing moved.
    bool takeResult(Result& out);

    Snapshot snapshot() const;

    // Forget the stream: a new binding, or a new source. Blocks until the
    // worker is idle, so nothing decoded against the old epoch can arrive
    // afterwards and be drawn as if it belonged to the new one.
    void reset();

  private:
    void run();

    // Decode `position` out of the window the worker holds. Returns the picture
    // through the staging slot. Runs on the worker thread only.
    void decodeCurrent(double position, bool complete, std::uint64_t sequence);

    // Whether a newer request has arrived, which makes everything this call is
    // doing wasted work. Read without the lock -- it is a hint that is allowed
    // to be one access unit stale.
    bool superseded(std::uint64_t sequence) const { return sequence_.load() != sequence; }

    mutable std::mutex mutex_;
    std::condition_variable work_;
    std::thread thread_;
    bool stopping_ = false;

    // The request waiting to be picked up, if any. One slot, not a queue.
    bool has_pending_ = false;
    Request pending_;

    // Bumped by every request, so a decode in flight can notice it is stale.
    std::atomic<std::uint64_t> sequence_{0};

    bool has_result_ = false;
    Result result_;

    // Snapshot fields, written by the worker after every decode.
    Snapshot snapshot_;

    bool hardware_enabled_ = true;
    bool hardware_pending_ = false;

    bool reset_requested_ = false;
    bool idle_ = true;
    std::condition_variable idle_cv_;

    // ---------------------------------------------- worker thread state only

    VideoDecoder decoder_;

    // The window being decoded through, owned by the worker.
    std::vector<Unit> window_;
    std::uint64_t window_id_ = 0;
    bool window_valid_ = false;

    // Progress, as a TIME rather than an index -- an index would shift under a
    // window that grew at the front, and the live one grows.
    double decoded_through_ = 0.0;
    bool anything_decoded_ = false;

    double last_position_ = 0.0;

    // What the last COMPLETED pass answered. Re-running an identical request
    // cannot produce a different picture, and at the end of a recording it would
    // never stop -- see the guard at the top of decodeCurrent().
    bool settled_ = false;
    std::uint64_t settled_window_id_ = 0;
    std::size_t settled_units_ = 0;
    double settled_position_ = 0.0;
};

}  // namespace scope

#endif  // SCOPE_VIDEO_DECODE_WORKER_H_
