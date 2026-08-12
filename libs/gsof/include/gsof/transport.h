// SPDX-License-Identifier: GPL-3.0-or-later
//
// Reassembles the multi-page transmissions that carry GSOF records and
// application files.
//
// The DATA of a GENOUT (0x40) or APPFILE (0x64) packet starts with three bytes:
//
//     TX_NUM | PAGE_INDEX | MAX_PAGE_INDEX | ...record bytes...
//
// TX_NUM is constant across the pages of one transmission and increments
// between transmissions. PAGE_INDEX runs 0..MAX_PAGE_INDEX. A single-page
// transmission has MAX_PAGE_INDEX == 0.
//
// The reason this is a separate stage rather than something the record parser
// does inline: RECORDS CAN STRADDLE A PAGE BOUNDARY. A page is capped at 252
// record bytes, a detailed-satellite record for a full constellation is
// larger, and the receiver splits it mid-record without regard for record
// framing. So every page's records must be concatenated before any record
// header is read. Parsing per page produces a parser that works perfectly on
// every small record and corrupts exactly the big ones.
//
// Because GENOUT and APPFILE share this header, one assembler serves the
// report stream and the configuration read-back both.

#ifndef GSOF_TRANSPORT_H
#define GSOF_TRANSPORT_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "gsof/error.h"

namespace gsof
{

// The three-byte header at the front of a GENOUT/APPFILE payload.
struct TransportHeader
{
    std::uint8_t transmissionNumber { 0 };
    std::uint8_t pageIndex { 0 };
    std::uint8_t maxPageIndex { 0 };
};

inline constexpr std::size_t kTransportHeaderSize = 3;

// Split a packet's DATA into its transport header and its record bytes.
constexpr Result<TransportHeader> parse_transport_header(std::span<const std::uint8_t> data)
{
    if (data.size() < kTransportHeaderSize)
    {
        return truncated(static_cast<std::uint16_t>(data.size()));
    }

    return TransportHeader { data[0], data[1], data[2] };
}

class PageAssembler
{
  public:
    enum class Feed
    {
        // Buffered; more pages are needed before there is anything to parse.
        Incomplete,
        // payload() now holds a whole transmission's record bytes.
        Complete,
    };

    struct Stats
    {
        // Transmissions handed out complete.
        std::uint64_t transmissions { 0 };
        // Pages buffered towards a transmission that was then abandoned --
        // because a page arrived out of order, or because a new transmission
        // started before the old one finished. Non-zero means pages are being
        // lost somewhere upstream.
        std::uint64_t pagesDiscarded { 0 };
        // Times a partial transmission was thrown away and a new one started.
        std::uint64_t restarts { 0 };
        // Times the accumulated payload hit its cap.
        std::uint64_t overflows { 0 };
    };

    // 256 pages of 252 record bytes is the protocol maximum, so this bound is
    // generous rather than restrictive. It exists only so a receiver that
    // never sends a final page cannot grow the buffer forever.
    static constexpr std::size_t kDefaultMaxPayload = 64 * 1024;

    explicit PageAssembler(std::size_t maxPayload = kDefaultMaxPayload);

    // Feed one packet's DATA, transport header included.
    //
    // On PageOutOfOrder the partial transmission is discarded, so the next
    // page 0 to arrive starts cleanly -- an assembler that kept the fragments
    // would concatenate across the gap and hand out a payload whose records
    // are silently spliced.
    Result<Feed> feed(std::span<const std::uint8_t> data);

    // Valid after feed() returns Complete, until the next feed().
    std::span<const std::uint8_t> payload() const { return mPayload; }

    // The transport header of the transmission currently held.
    const TransportHeader& header() const { return mHeader; }

    void reset();

    const Stats& stats() const { return mStats; }

  private:
    void discardPartial();

    std::vector<std::uint8_t> mPayload;
    TransportHeader mHeader {};
    // Whether mPayload holds pages of an unfinished transmission.
    bool mAssembling { false };
    std::size_t mPagesHeld { 0 };
    std::size_t mMaxPayload;
    Stats mStats {};
};

} // namespace gsof

#endif // GSOF_TRANSPORT_H
