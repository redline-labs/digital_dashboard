#ifndef PUB_SUB_CAPNP_PAYLOAD_H_
#define PUB_SUB_CAPNP_PAYLOAD_H_

#include <capnp/common.h>

#include <kj/array.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace pub_sub
{

// Word-aligned access to a byte payload, for capnp::FlatArrayMessageReader.
//
// capnp reads whole 8-byte words, and FlatArrayMessageReader takes an
// ArrayPtr<const word>. A zenoh payload is a byte buffer: neither its length nor
// its address is guaranteed to be a multiple of 8. This tree used to bridge that
// gap in five places by casting the byte pointer straight to `const word*`, and
// four of those five guarded it with `size % 8 == 0` -- which tests the length,
// not the address, and so checked nothing about alignment at all.
//
// Those casts were correct only by accident: every one of them is handed a fresh
// std::vector<uint8_t> from zenoh's as_vector(), and operator new returns memory
// aligned for max_align_t, so the payload was always word-aligned in practice.
// Nothing said so. Feed any of those sites a sub-range, an mmap'd buffer, or a
// zero-copy view instead and capnp's own guard fires --
//
//     capnp/arena.c++:83: failed: ... % sizeof(void*) == 0 [1 == 0];
//     Detected unaligned data in Cap'n Proto message.
//
// -- which is a thrown kj::Exception, so the sample is dropped rather than the
// process crashing. (capnp checks because the undefined behaviour is real: it
// notes that GCC emits SIMD in optimised builds and those instructions require
// alignment.) This class makes the alignment true by construction instead of by
// coincidence.
//
// The copy only happens when the payload really is misaligned, so the common
// case stays zero-copy. This mirrors what capnp 2.0's own
// FlatArrayMessageReader(ArrayPtr<const byte>) overload does internally; when we
// eventually move to it, every use of this class collapses into passing the
// bytes directly.
//
// The reader built from words() borrows from this object, so keep it alive for
// as long as the reader:
//
//     const WordAlignedPayload aligned(bytes);
//     if (aligned.empty()) { /* refuse the sample */ }
//     capnp::FlatArrayMessageReader reader(aligned.words());
class WordAlignedPayload
{
  public:
    explicit WordAlignedPayload(const kj::byte* data, std::size_t size)
    {
        // A partial word cannot be completed, and capnp would read the short
        // buffer as a message whose fields are all default -- so a damaged
        // packet would decode exactly like a valid one reporting zero. Refuse it
        // instead, and let the caller say so in its own terms.
        if (size == 0 || (size % sizeof(capnp::word)) != 0)
        {
            return;
        }

        const std::size_t word_count = size / sizeof(capnp::word);

        if ((reinterpret_cast<std::uintptr_t>(data) % sizeof(capnp::word)) == 0)
        {
            words_ = kj::arrayPtr(reinterpret_cast<const capnp::word*>(data), word_count);
            return;
        }

        owned_ = kj::heapArray<capnp::word>(word_count);
        std::memcpy(owned_.begin(), data, size);
        words_ = owned_.asPtr();
    }

    explicit WordAlignedPayload(kj::ArrayPtr<const kj::byte> bytes) :
        WordAlignedPayload(bytes.begin(), bytes.size())
    {
    }

    // What zenoh's Bytes::as_vector() hands back, which is how every subscriber
    // and queryable in this tree receives a payload.
    explicit WordAlignedPayload(const std::vector<std::uint8_t>& bytes) :
        WordAlignedPayload(reinterpret_cast<const kj::byte*>(bytes.data()), bytes.size())
    {
    }

    // Empty when the payload was not a whole number of words, including when it
    // was zero-length. Every caller has to handle this: passing an empty view to
    // FlatArrayMessageReader yields a message reading as all-defaults.
    kj::ArrayPtr<const capnp::word> words() const { return words_; }
    bool empty() const { return words_.size() == 0; }

  private:
    // Set only when the payload had to be copied; words() points into it then,
    // and straight at the caller's buffer otherwise.
    kj::Array<capnp::word> owned_;
    kj::ArrayPtr<const capnp::word> words_;
};

}  // namespace pub_sub

#endif  // PUB_SUB_CAPNP_PAYLOAD_H_
