#include "video/video_decode_worker.h"

#include <spdlog/spdlog.h>

#include <limits>
#include <utility>

namespace scope
{

namespace
{

// How often a running decode checks whether it has been superseded.
//
// Every access unit would be an atomic load per millisecond of work, which is
// nothing -- but the check also costs a chance to finish, and abandoning a
// catch-up that was two units from done to start it again from the keyframe is
// the one way this could be slower than not checking at all.
constexpr std::size_t kSupersedeCheckInterval = 8;

}  // namespace

VideoDecodeWorker::VideoDecodeWorker()
{
    thread_ = std::thread([this]() { run(); });
}

VideoDecodeWorker::~VideoDecodeWorker()
{
    {
        const std::lock_guard<std::mutex> guard(mutex_);
        stopping_ = true;
    }
    work_.notify_all();
    if (thread_.joinable())
    {
        thread_.join();
    }
}

void VideoDecodeWorker::setHardwareEnabled(bool on)
{
    {
        const std::lock_guard<std::mutex> guard(mutex_);
        if (hardware_enabled_ == on)
        {
            return;
        }
        hardware_enabled_ = on;
        hardware_pending_ = true;
    }
    work_.notify_one();
}

void VideoDecodeWorker::request(Request request)
{
    {
        const std::lock_guard<std::mutex> guard(mutex_);

        // REPLACING, not queueing. A drag makes one of these per render tick and
        // every one of them supersedes the last: decoding a position the pointer
        // has already left is work that can only make the picture later.
        //
        // An APPEND may not be dropped, though -- its units are the only copy
        // the worker will ever be offered. So an append onto a pending request
        // for the same window merges into it rather than replacing it.
        if (has_pending_ && !request.replace && pending_.window_id == request.window_id)
        {
            pending_.units.insert(pending_.units.end(),
                                  std::make_move_iterator(request.units.begin()),
                                  std::make_move_iterator(request.units.end()));
            pending_.position = request.position;
        }
        else
        {
            pending_ = std::move(request);
            has_pending_ = true;
        }

        sequence_.fetch_add(1);
    }
    work_.notify_one();
}

bool VideoDecodeWorker::takeResult(Result& out)
{
    const std::lock_guard<std::mutex> guard(mutex_);
    if (!has_result_)
    {
        return false;
    }
    out = std::move(result_);
    result_ = Result{};
    has_result_ = false;
    return true;
}

VideoDecodeWorker::Snapshot VideoDecodeWorker::snapshot() const
{
    const std::lock_guard<std::mutex> guard(mutex_);
    return snapshot_;
}

void VideoDecodeWorker::reset()
{
    std::unique_lock<std::mutex> guard(mutex_);

    has_pending_ = false;
    pending_ = Request{};
    has_result_ = false;
    result_ = Result{};
    reset_requested_ = true;
    sequence_.fetch_add(1);

    // BLOCKS UNTIL IDLE. Everything the worker is holding belongs to the stream
    // being dropped, and a picture that landed after the panel rebound would be
    // drawn against the new source's clock -- a frame from one recording,
    // stamped with an instant in another.
    work_.notify_one();
    idle_cv_.wait(guard, [this]() { return idle_ || stopping_; });
}

void VideoDecodeWorker::run()
{
    std::unique_lock<std::mutex> guard(mutex_);

    while (!stopping_)
    {
        if (reset_requested_)
        {
            reset_requested_ = false;
            guard.unlock();
            decoder_.reset();
            decoder_.clearImage();
            decoder_.resetStats();
            window_.clear();
            window_valid_ = false;
            anything_decoded_ = false;
            guard.lock();
            snapshot_ = Snapshot{};
            continue;
        }

        if (hardware_pending_)
        {
            hardware_pending_ = false;
            const bool on = hardware_enabled_;
            guard.unlock();

            // Closes the decoder, so the window has to be fed again from its
            // keyframe -- there is no decoder left holding the references the
            // rest of it was decoded against.
            decoder_.setHardwareEnabled(on);
            window_valid_ = false;
            anything_decoded_ = false;

            guard.lock();
            continue;
        }

        if (!has_pending_)
        {
            idle_ = true;
            idle_cv_.notify_all();
            work_.wait(guard);
            continue;
        }

        Request request = std::move(pending_);
        pending_ = Request{};
        has_pending_ = false;
        idle_ = false;
        const std::uint64_t sequence = sequence_.load();

        guard.unlock();

        // Take the window on. Replacing means a different stretch of the
        // recording, so nothing already decoded applies to it.
        if (request.replace)
        {
            window_ = std::move(request.units);
            window_id_ = request.window_id;
            window_valid_ = false;
        }
        else
        {
            if (request.window_id != window_id_)
            {
                // An append for a window this worker never received. Only
                // possible if a reset landed between the panel deciding to
                // append and this running; the units are dropped and the panel
                // ships the whole window again on its next tick.
                window_.clear();
                window_id_ = request.window_id;
                window_valid_ = false;
            }
            window_.insert(window_.end(), std::make_move_iterator(request.units.begin()),
                           std::make_move_iterator(request.units.end()));
        }

        // Threading follows what the window is for, and changing it closes the
        // decoder -- so this is checked before the feed rather than during it.
        if (request.seek_optimised != decoder_.seekOptimised())
        {
            decoder_.setSeekOptimised(request.seek_optimised);
            window_valid_ = false;
            anything_decoded_ = false;
        }

        decodeCurrent(request.position, request.complete, sequence);

        guard.lock();
    }

    idle_ = true;
    idle_cv_.notify_all();
}

void VideoDecodeWorker::decodeCurrent(double position, bool complete, std::uint64_t sequence)
{
    // A RESTART, on any of three counts: a window that was replaced, a position
    // that moved backwards -- a decoder cannot un-consume the frames it has been
    // given -- or a decoder left flushed by a drain.
    // NOTHING CAN HAVE CHANGED SINCE THE LAST PASS: same window, same units in
    // it, same instant, and that pass ran the catch-up to completion. Whatever
    // it produced is already on screen.
    //
    // This is not merely an optimisation, it is what stops a loop. At the END of
    // a recording the target is never overshot -- there is no later frame to
    // overshoot it with -- so every pass finishes by draining, and a drain
    // leaves the decoder flushed, which forces the next pass to restart. Without
    // this the panel re-decodes the last GOP thirty times a second for as long
    // as it sits there, showing the same frame throughout: invisible, and a
    // whole core.
    if (settled_ && window_id_ == settled_window_id_ && window_.size() == settled_units_ &&
        position == settled_position_)
    {
        return;
    }

    const bool restart = !window_valid_ || position < last_position_;
    if (restart)
    {
        decoder_.reset();
        anything_decoded_ = false;
        window_valid_ = true;
    }
    last_position_ = position;

    decoder_.setTarget(position);

    std::size_t index = 0;
    if (anything_decoded_)
    {
        while (index < window_.size() && window_[index].t <= decoded_through_)
        {
            ++index;
        }
    }

    // PAST THE PLAYHEAD IS ALLOWED, until the decoder overshoots it. A decoder
    // with delay holds the frame for `position` back until it has the packets
    // that follow it, so stopping exactly at the playhead would ask for a frame
    // and never be given it. Feeding on is not showing on: those frames are
    // stashed against the target advancing onto them.
    std::size_t since_check = 0;
    while (index < window_.size() &&
           (window_[index].t <= position || !decoder_.reachedTarget()))
    {
        if (++since_check >= kSupersedeCheckInterval)
        {
            since_check = 0;
            if (superseded(sequence))
            {
                // A newer position is waiting. Everything decoded so far is
                // kept -- it is real progress through this window -- but nothing
                // is presented, because the instant it answers is one the user
                // has already left.
                return;
            }
        }

        const Unit& unit = window_[index];
        ++index;

        VideoDecoder::AccessUnit access;
        access.data = std::span<const std::uint8_t>(unit.data.data(), unit.data.size());
        access.codec = unit.h265 ? VideoDecoder::Codec::H265 : VideoDecoder::Codec::H264;
        access.is_config = unit.is_config;
        access.is_keyframe = unit.is_keyframe;
        access.t = unit.t;

        decoder_.submit(access);
        decoded_through_ = unit.t;
        anything_decoded_ = true;
    }

    const bool exhausted = index >= window_.size();

    // EVERYTHING THERE IS HAS BEEN FED AND THE TARGET WAS NEVER OVERSHOT, so
    // the frame asked for may still be inside libavcodec. Only a decoder with
    // delay gets here; draining pushes it out.
    //
    // `complete` is what makes this safe to do. On a recording the window is a
    // whole GOP and nothing more is coming, so the frame is either drained out
    // or never shown. On a LIVE window the same state just means the next unit
    // has not arrived yet -- and draining flushes the decoder, which would
    // re-decode the whole GOP on the next tick, and the tick after that.
    if (exhausted && complete && !decoder_.reachedTarget())
    {
        decoder_.drain();

        // Drained leaves it flushed, so the window has to be re-fed from its
        // keyframe if anything is asked of it again.
        window_valid_ = false;
    }

    // ONLY WHEN THE CATCH-UP IS DONE, which off the GUI thread is the ordinary
    // case rather than the lucky one -- nothing interrupts this but a newer
    // request, and a newer request means the answer was going to be stale.
    const bool caught_up = decoder_.reachedTarget() || exhausted;
    const bool drew = caught_up && decoder_.present();

    settled_ = caught_up;
    settled_window_id_ = window_id_;
    settled_units_ = window_.size();
    settled_position_ = position;

    const std::lock_guard<std::mutex> guard(mutex_);

    snapshot_.stats = decoder_.stats();
    snapshot_.synced = decoder_.synced();
    snapshot_.backend = decoder_.backend();

    if (drew)
    {
        // QImage is implicitly shared and copy-on-write, so this hands the panel
        // a reference rather than a picture -- but the decoder reuses its own
        // buffer for the NEXT frame, so the copy has to be forced here. Without
        // it the panel would be painting a picture the worker is overwriting.
        result_.image = decoder_.image().copy();
        result_.frame_t = decoder_.frameTime();
        result_.position = position;
        has_result_ = true;
    }
}

}  // namespace scope
