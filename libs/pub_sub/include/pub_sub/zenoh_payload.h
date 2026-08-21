#ifndef PUB_SUB_ZENOH_PAYLOAD_H_
#define PUB_SUB_ZENOH_PAYLOAD_H_

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <capnp/common.h>
#include <kj/array.h>
#include <zenoh.hxx>

namespace pub_sub
{

// A capnp word view of a zenoh payload, WITHOUT copying it.
//
// Every receiver in this tree used to start with `sample.get_payload().as_vector()`,
// which allocates a std::vector the size of the message and memcpy's the whole
// payload into it purely so capnp has something word-aligned to read. capnp's
// reading is genuinely zero-copy -- walking a decoded 9.4 MB tile batch and
// summing every element measures 5 us -- so that copy was the entire decode
// cost and it bought nothing.
//
// MEASURED on a 64-tile map reply: as_vector() is 293 us of a 9.4 MB response
// and 23 us of an 888 KB one. That was ~14% of the whole round trip once the
// bigger problems above it were gone.
//
// WHAT IT COSTS INSTEAD: nothing, in the case that actually occurs. zenoh hands
// back one contiguous slice, `words()` points straight at zenoh's own buffer,
// and the sample stays alive for the duration of the callback that owns it.
//
// THE TWO CASES THAT ARE NOT FREE, both preserved rather than assumed away:
//
//   several slices -- zenoh may hold a payload in more than one region, and a
//     capnp message has to be contiguous, so those are joined here. That is the
//     same single copy as before, no worse.
//   a misaligned slice -- capnp requires word alignment and checks for it
//     (arena.c++ raises "Detected unaligned data in Cap'n Proto message"), so a
//     slice that does not start on an 8-byte boundary is copied to one that
//     does. In practice zenoh's buffers are aligned and this never fires, which
//     is exactly why it must be handled rather than trusted -- see the same
//     reasoning in capnp_payload.h, whose WordAlignedPayload does this job for
//     buffers that did NOT come from a live zenoh sample (bag replay, recorded
//     sources, tests).
//
// LIFETIME: in the borrowed case this points into the zenoh sample. Keep the
// sample alive for as long as the reader built from words():
//
//     const ZenohPayload payload(sample.get_payload());
//     if (payload.empty()) { /* refuse it */ }
//     capnp::FlatArrayMessageReader reader(payload.words());
class ZenohPayload
{
  public:
    explicit ZenohPayload(const zenoh::Bytes& payload)
    {
        const std::size_t total = payload.size();

        // A partial word cannot be completed, and capnp would read the short
        // buffer as a message whose fields are all default -- so a damaged
        // packet decodes exactly like a valid one reporting zero. Refuse it and
        // let the caller say so in its own terms.
        if (total == 0 || (total % sizeof(capnp::word)) != 0)
        {
            return;
        }

        // The common case: one slice, already aligned, so borrow it.
        auto first = payload.slice_iter();
        if (auto slice = first.next())
        {
            const bool only_slice = !first.next().has_value();
            const bool aligned =
                (reinterpret_cast<std::uintptr_t>(slice->data) % sizeof(capnp::word)) == 0;
            if (only_slice && aligned && slice->len == total)
            {
                words_ = kj::arrayPtr(reinterpret_cast<const capnp::word*>(slice->data),
                                      total / sizeof(capnp::word));
                return;
            }
        }

        // Fragmented or misaligned: join into one aligned buffer. kj::heapArray
        // is allocated for capnp::word, so the result is aligned by construction.
        joined_ = kj::heapArray<capnp::word>(total / sizeof(capnp::word));
        auto* out = reinterpret_cast<std::uint8_t*>(joined_.begin());
        std::size_t written = 0;
        auto it = payload.slice_iter();
        while (auto slice = it.next())
        {
            if (written + slice->len > total)
            {
                // The slices do not agree with size(). Refuse rather than
                // decode a message assembled from a buffer we mis-measured.
                joined_ = nullptr;
                return;
            }
            std::memcpy(out + written, slice->data, slice->len);
            written += slice->len;
        }
        if (written != total)
        {
            joined_ = nullptr;
            return;
        }
        words_ = joined_.asPtr();
    }

    ZenohPayload(const ZenohPayload&) = delete;
    ZenohPayload& operator=(const ZenohPayload&) = delete;

    // Empty when the payload was not a whole number of words, including when it
    // was zero-length, and when the slices did not add up. Every caller has to
    // handle it: passing an empty view to FlatArrayMessageReader yields a
    // message that reads as all-defaults.
    kj::ArrayPtr<const capnp::word> words() const { return words_; }
    bool empty() const { return words_.size() == 0; }

  private:
    // Set only when the payload had to be joined or realigned; words() points
    // into it then, and straight into the zenoh sample otherwise.
    kj::Array<capnp::word> joined_;
    kj::ArrayPtr<const capnp::word> words_;
};

}  // namespace pub_sub

#endif  // PUB_SUB_ZENOH_PAYLOAD_H_
