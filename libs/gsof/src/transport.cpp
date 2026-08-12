// SPDX-License-Identifier: GPL-3.0-or-later

#include "gsof/transport.h"

#include <algorithm>

namespace gsof
{

PageAssembler::PageAssembler(std::size_t maxPayload) :
    mMaxPayload(std::max<std::size_t>(maxPayload, 256))
{
}

void PageAssembler::discardPartial()
{
    if (mAssembling)
    {
        mStats.pagesDiscarded += mPagesHeld;
        ++mStats.restarts;
    }

    mPayload.clear();
    mPagesHeld = 0;
    mAssembling = false;
}

Result<PageAssembler::Feed> PageAssembler::feed(std::span<const std::uint8_t> data)
{
    const Result<TransportHeader> header = parse_transport_header(data);
    if (!header.has_value())
    {
        return std::unexpected(header.error());
    }

    const std::span<const std::uint8_t> records = data.subspan(kTransportHeaderSize);

    const bool continues = mAssembling &&
                           header->transmissionNumber == mHeader.transmissionNumber &&
                           header->maxPageIndex == mHeader.maxPageIndex &&
                           header->pageIndex == static_cast<std::uint8_t>(mHeader.pageIndex + 1);

    if (!continues)
    {
        // Anything that is not the next page of what we hold starts over. Two
        // cases land here and both are correct to restart on: page 0 of a new
        // transmission (the normal path), and a page that skipped, repeated or
        // changed transmission number (a genuine gap).
        const bool startsFresh = header->pageIndex == 0;

        discardPartial();

        if (!startsFresh)
        {
            // Report it, but having already discarded: the next page 0 will be
            // accepted, so one bad page costs one transmission rather than
            // every transmission after it.
            return page_out_of_order(header->pageIndex);
        }
    }

    if (mPayload.size() + records.size() > mMaxPayload)
    {
        ++mStats.overflows;
        discardPartial();
        return too_long(static_cast<std::uint16_t>(mPayload.size()));
    }

    // Single-page transmissions are copied rather than returned as a view onto
    // the caller's buffer. It costs a memcpy of at most 252 bytes at the
    // output rate, and it buys one lifetime rule for payload() instead of two
    // -- "valid until the next feed()" regardless of how the transmission
    // arrived. A view that is sometimes into our buffer and sometimes into the
    // framer's is the kind of distinction that holds until the one call site
    // that keeps it a moment too long.
    mPayload.insert(mPayload.end(), records.begin(), records.end());
    ++mPagesHeld;
    mHeader = *header;

    if (header->pageIndex >= header->maxPageIndex)
    {
        mAssembling = false;
        mPagesHeld = 0;
        ++mStats.transmissions;
        return Feed::Complete;
    }

    mAssembling = true;
    return Feed::Incomplete;
}

void PageAssembler::reset()
{
    mPayload.clear();
    mPagesHeld = 0;
    mAssembling = false;
}

} // namespace gsof
