// SPDX-License-Identifier: GPL-3.0-or-later
//
// The type-length-value walk that Trimble uses twice.
//
// GSOF records inside a GENOUT payload and application-file records inside an
// APPFILE payload have exactly the same framing -- TYPE | LENGTH | BODY, back
// to back, no terminator and no count, with LENGTH excluding the two header
// bytes. They are different vocabularies over one grammar, so the grammar
// lives here and each vocabulary is a table elsewhere.
//
// The framing being self-describing is what lets an unrecognised type be
// SKIPPED rather than being fatal: the length byte says how far to jump even
// when nothing here knows what the body means. A walk that stopped at the
// first unknown type would lose every record behind it -- so on the GSOF side,
// enabling one new message on the receiver would silently disable the ones
// after it, and on the application-file side, one unfamiliar setting would
// hide the rest of the receiver's configuration.

#ifndef GSOF_TLV_H
#define GSOF_TLV_H

#include <cstddef>
#include <cstdint>
#include <span>

#include "gsof/error.h"

namespace gsof
{

// TYPE and LENGTH ahead of every body.
inline constexpr std::size_t kTlvHeaderSize = 2;

// One record, still as bytes. `body` points into the payload it came from.
struct TlvRecord
{
    std::uint8_t type { 0 };
    std::span<const std::uint8_t> body;
};

// A malformed record stops the walk: once a length byte cannot be trusted,
// every offset after it is a guess.
class TlvIterator
{
  public:
    explicit constexpr TlvIterator(std::span<const std::uint8_t> payload) : mPayload(payload) {}

    // The next record, or an error. Call done() rather than testing for the
    // Truncated that ends the walk.
    constexpr Result<TlvRecord> next()
    {
        if (mOffset + kTlvHeaderSize > mPayload.size())
        {
            return truncated(static_cast<std::uint16_t>(mOffset));
        }

        const std::uint8_t type = mPayload[mOffset];
        const std::size_t length = mPayload[mOffset + 1];
        const std::size_t bodyAt = mOffset + kTlvHeaderSize;

        if (bodyAt + length > mPayload.size())
        {
            // The record claims more bytes than the payload holds. On the GSOF
            // side this is the signature of a page lost in reassembly: the
            // payload is short, and the last record is the one that notices.
            // Stop -- there is nothing after this that can be trusted.
            mOffset = mPayload.size();
            return length_mismatch(type, static_cast<std::uint16_t>(bodyAt));
        }

        mOffset = bodyAt + length;
        return TlvRecord { type, mPayload.subspan(bodyAt, length) };
    }

    constexpr bool done() const { return mOffset + kTlvHeaderSize > mPayload.size(); }

    // Bytes consumed so far. A value short of the payload size after done()
    // means a trailing fragment too small to be a record.
    constexpr std::size_t offset() const { return mOffset; }

  private:
    std::span<const std::uint8_t> mPayload;
    std::size_t mOffset { 0 };
};

} // namespace gsof

#endif // GSOF_TLV_H
