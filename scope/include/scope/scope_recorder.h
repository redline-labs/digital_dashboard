#ifndef SCOPE_SCOPE_RECORDER_H_
#define SCOPE_SCOPE_RECORDER_H_

#include "scope/capture_buffer.h"
#include "scope/recorded_source.h"

#include <cstdint>
#include <memory>
#include <string>

namespace scope
{

// Captures the WHOLE bus into a CaptureBuffer while scope is live.
//
// EVERYTHING, with no exclusions. The point of capturing is that a signal
// nobody thought to plot can still be added afterwards -- a filter set from the
// panels would only ever record what was already on screen, which is exactly
// what you do not need after the fact.
//
// The rules below come from nodes/bag/record.cpp and are not optional:
//
//   - The ARRIVAL TIMESTAMP is taken in the zenoh callback, not later. A
//     consumer that is backing up would otherwise fold its own latency into
//     every log_time, and the recording's timing would slew under load in a way
//     that is invisible afterwards.
//   - The callback COPIES AND RETURNS. It runs on a zenoh RX thread; stalling
//     one stalls the session for everything, including the liveliness traffic
//     the signal browser depends on. An exception crossing the Rust FFI
//     boundary is fatal to the process.
//   - TEARDOWN ORDER is subscriber first, then buffer. The subscriber's
//     destructor joins in-flight callbacks; the reverse order lets a callback
//     write into a buffer that is being destroyed.
class ScopeRecorder
{
  public:
    ScopeRecorder(std::size_t max_bytes, double max_seconds);
    ~ScopeRecorder();

    ScopeRecorder(const ScopeRecorder&) = delete;
    ScopeRecorder& operator=(const ScopeRecorder&) = delete;

    // False when the subscription could not be declared -- no bus, or a session
    // that would not open. The window keeps working; it simply has no capture.
    bool isValid() const;

    CaptureBuffer& buffer();
    const CaptureBuffer& buffer() const;

    // Messages seen since construction, including those since evicted.
    std::uint64_t received() const;

    // Writes everything retained into `directory` as an ordinary bag.
    //
    // Going through bag::BagWriter rather than a private format is the whole
    // point: the result opens in `bag info`, `bag verify`, `bag play` and
    // Foxglove, and carries the schema descriptors and drop counts the writer
    // already knows how to record. A capture our own CLI refused would not be a
    // capture.
    //
    // The eviction count is recorded as the bag's dropped_messages, because
    // that is exactly what it is from the file's point of view.
    bool saveTo(const std::string& directory) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// A RecordedProvider over a live CaptureBuffer, so reviewing the capture and
// reviewing a bag on disk are ONE DataSource implementation.
//
// The buffer must outlive this. The span moves as the capture continues and as
// its head is evicted -- revision() is what tells the transport bar the scrubber
// needs a new range.
class CaptureProvider : public RecordedProvider
{
  public:
    explicit CaptureProvider(const CaptureBuffer& buffer);

    void forEach(std::uint64_t t0_ns, std::uint64_t t1_ns,
                 const std::function<void(const bag::BagMessage&)>& visit) override;
    std::vector<TopicInfo> topics() const override;
    std::pair<std::uint64_t, std::uint64_t> spanNanos() const override;
    std::uint64_t revision() const override;

    // Exact, unlike the bag's part-index approximation: the buffer holds every
    // message and can count them. Still not per frame -- it walks the deque
    // under the mutex the RX thread needs to push. See CaptureBuffer::density().
    bool density(std::uint64_t t0_ns, std::uint64_t t1_ns, std::size_t buckets,
                 std::vector<std::uint32_t>& out) override;

  private:
    const CaptureBuffer* buffer_;
};

}  // namespace scope

#endif  // SCOPE_SCOPE_RECORDER_H_
