// SPDX-License-Identifier: GPL-3.0-or-later
//
// Multi-page reassembly.
//
// The case that justifies this stage existing at all is `test_record_split_
// across_a_page_boundary`: the receiver fills a page to 252 bytes and continues
// the record it was in the middle of on the next one, with no regard for
// record framing. A parser that reads records per page therefore works
// perfectly on every small record and corrupts exactly the large ones -- which,
// for a GNSS receiver, means the satellite detail records and nothing else.
// That is a bug you would chase for a long time.
//
// The rest of the file is about a gap in the stream, which for a page
// assembler is the interesting failure: pages are concatenated, so a missing
// page does not truncate a payload, it SPLICES two halves of different records
// together and hands out something that parses.

#include "gsof/transport.h"

#include <spdlog/spdlog.h>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace
{

int failures = 0;

void check(bool condition, const std::string& what)
{
    if (!condition)
    {
        SPDLOG_ERROR("FAIL: {}", what);
        ++failures;
    }
}

using namespace gsof;

// One packet's DATA: the three-byte transport header followed by record bytes.
std::vector<std::uint8_t> page(std::uint8_t txNum, std::uint8_t pageIndex, std::uint8_t maxPageIndex,
                               const std::vector<std::uint8_t>& records)
{
    std::vector<std::uint8_t> data { txNum, pageIndex, maxPageIndex };
    data.insert(data.end(), records.begin(), records.end());
    return data;
}

std::vector<std::uint8_t> collected(const PageAssembler& assembler)
{
    const std::span<const std::uint8_t> payload = assembler.payload();
    return std::vector<std::uint8_t>(payload.begin(), payload.end());
}

// ============================================================================
// The transport header itself
// ============================================================================

// A three-byte payload is the smallest legal one: header, no records.
constexpr std::array<std::uint8_t, 3> kHeaderOnly { 0x2A, 0x00, 0x00 };
static_assert(parse_transport_header(kHeaderOnly).has_value());
static_assert(parse_transport_header(kHeaderOnly)->transmissionNumber == 0x2A);
static_assert(parse_transport_header(kHeaderOnly)->pageIndex == 0);
static_assert(parse_transport_header(kHeaderOnly)->maxPageIndex == 0);

constexpr std::array<std::uint8_t, 2> kTooShort { 0x2A, 0x00 };
static_assert(parse_transport_header(kTooShort).error().kind == ErrorKind::Truncated);

// ============================================================================
// Single page
// ============================================================================

void test_single_page_completes_immediately()
{
    PageAssembler assembler;

    const Result<PageAssembler::Feed> fed = assembler.feed(page(0x10, 0, 0, { 0x01, 0x02, 0x03 }));

    check(fed.has_value() && *fed == PageAssembler::Feed::Complete,
          "a transmission whose max page index is zero completes on its only page");
    check(collected(assembler) == std::vector<std::uint8_t> { 0x01, 0x02, 0x03 },
          "and the payload is the record bytes with the transport header stripped");
    check(assembler.header().transmissionNumber == 0x10, "the transmission number is reported");
    check(assembler.stats().transmissions == 1, "one transmission counted");
    check(assembler.stats().restarts == 0, "and nothing was discarded");
}

void test_a_page_with_no_records_is_legal()
{
    PageAssembler assembler;

    const Result<PageAssembler::Feed> fed = assembler.feed(page(0x11, 0, 0, {}));

    check(fed.has_value() && *fed == PageAssembler::Feed::Complete,
          "a transmission carrying no records still completes");
    check(assembler.payload().empty(), "with an empty payload");
}

void test_header_shorter_than_three_bytes_is_truncated()
{
    PageAssembler assembler;

    const std::vector<std::uint8_t> stub { 0x11, 0x00 };
    const Result<PageAssembler::Feed> fed = assembler.feed(stub);

    check(!fed.has_value() && fed.error().kind == ErrorKind::Truncated,
          "a payload too short to hold the transport header is refused");
}

// ============================================================================
// Multiple pages
// ============================================================================

void test_three_pages_reassemble_in_order()
{
    PageAssembler assembler;

    const Result<PageAssembler::Feed> first = assembler.feed(page(0x20, 0, 2, { 0xAA, 0xAB }));
    check(first.has_value() && *first == PageAssembler::Feed::Incomplete, "page 0 of 2 does not complete");

    const Result<PageAssembler::Feed> second = assembler.feed(page(0x20, 1, 2, { 0xBA, 0xBB }));
    check(second.has_value() && *second == PageAssembler::Feed::Incomplete, "page 1 of 2 does not complete");

    const Result<PageAssembler::Feed> third = assembler.feed(page(0x20, 2, 2, { 0xCA, 0xCB }));
    check(third.has_value() && *third == PageAssembler::Feed::Complete, "page 2 of 2 completes");

    check(collected(assembler) == std::vector<std::uint8_t> { 0xAA, 0xAB, 0xBA, 0xBB, 0xCA, 0xCB },
          "the pages concatenate in order with every transport header stripped");
    check(assembler.stats().transmissions == 1, "three pages are one transmission, not three");
}

void test_record_split_across_a_page_boundary()
{
    // THE reason this stage exists. A 300-byte record cannot fit in a page, so
    // it arrives as 252 bytes on one page and 48 on the next, split at an
    // arbitrary point that has nothing to do with record framing.
    PageAssembler assembler;

    std::vector<std::uint8_t> whole;
    whole.push_back(34);   // record type: detailed all-SV
    whole.push_back(255);  // record length
    for (int i = 0; i < 255; ++i)
    {
        whole.push_back(static_cast<std::uint8_t>(i));
    }

    const std::size_t cut = 252;
    const std::vector<std::uint8_t> firstHalf(whole.begin(), whole.begin() + cut);
    const std::vector<std::uint8_t> secondHalf(whole.begin() + cut, whole.end());

    const Result<PageAssembler::Feed> a = assembler.feed(page(0x30, 0, 1, firstHalf));
    check(a.has_value() && *a == PageAssembler::Feed::Incomplete, "the first half alone is not a transmission");

    const Result<PageAssembler::Feed> b = assembler.feed(page(0x30, 1, 1, secondHalf));
    check(b.has_value() && *b == PageAssembler::Feed::Complete, "the second half completes it");

    check(collected(assembler) == whole,
          "a record split mid-payload is reassembled byte for byte -- parse per page and this is corrupt");
}

void test_page_count_at_the_protocol_maximum()
{
    // 256 pages is the most the one-byte index can express. Feed all of them.
    PageAssembler assembler;

    std::vector<std::uint8_t> expected;
    Result<PageAssembler::Feed> fed = PageAssembler::Feed::Incomplete;

    for (int index = 0; index <= 255; ++index)
    {
        const std::vector<std::uint8_t> records { static_cast<std::uint8_t>(index),
                                                  static_cast<std::uint8_t>(index ^ 0xFF) };
        expected.insert(expected.end(), records.begin(), records.end());
        fed = assembler.feed(page(0x40, static_cast<std::uint8_t>(index), 255, records));
    }

    check(fed.has_value() && *fed == PageAssembler::Feed::Complete, "the 256th page completes the transmission");
    check(collected(assembler) == expected, "all 256 pages are present, in order");
}

// ============================================================================
// Gaps, duplicates and interleaving
// ============================================================================

void test_a_missing_page_is_reported_not_spliced()
{
    // Page 1 never arrives. Concatenating pages 0 and 2 would produce a
    // payload that still parses as records -- it would just be wrong, silently
    // and forever. So the whole transmission has to go.
    PageAssembler assembler;

    check(assembler.feed(page(0x50, 0, 2, { 0xAA, 0xAB })).has_value(), "page 0 accepted");

    const Result<PageAssembler::Feed> skipped = assembler.feed(page(0x50, 2, 2, { 0xCA, 0xCB }));
    check(!skipped.has_value() && skipped.error().kind == ErrorKind::PageOutOfOrder,
          "a page that skips one is refused");
    check(assembler.stats().transmissions == 0, "and no transmission is handed out");
    check(assembler.stats().restarts == 1, "the partial transmission was discarded");
    check(assembler.stats().pagesDiscarded == 1, "one page's worth of data was lost");

    // Recovery: the next transmission must be accepted normally.
    const Result<PageAssembler::Feed> next = assembler.feed(page(0x51, 0, 0, { 0x01 }));
    check(next.has_value() && *next == PageAssembler::Feed::Complete,
          "the assembler accepts the next transmission -- one gap costs one transmission, not all of them");
}

void test_a_duplicated_page_is_refused()
{
    PageAssembler assembler;

    check(assembler.feed(page(0x60, 0, 1, { 0xAA })).has_value(), "page 0 accepted");

    const Result<PageAssembler::Feed> repeat = assembler.feed(page(0x60, 0, 1, { 0xAA }));
    // A repeat of page 0 restarts cleanly rather than erroring -- page 0 is
    // always a legal place to begin, and treating a retransmitted first page
    // as an error would reject a recoverable stream.
    check(repeat.has_value() && *repeat == PageAssembler::Feed::Incomplete,
          "a repeated page 0 restarts the transmission rather than doubling the payload");
    check(assembler.stats().restarts == 1, "and the discard is counted");

    const Result<PageAssembler::Feed> finish = assembler.feed(page(0x60, 1, 1, { 0xBB }));
    check(finish.has_value() && *finish == PageAssembler::Feed::Complete, "and it then completes normally");
    check(collected(assembler) == std::vector<std::uint8_t> { 0xAA, 0xBB },
          "with the payload counted once, not twice");
}

void test_a_new_transmission_number_abandons_the_old_one()
{
    PageAssembler assembler;

    check(assembler.feed(page(0x70, 0, 1, { 0xAA })).has_value(), "page 0 of transmission 0x70 accepted");

    // The receiver moved on. Continuing to hold 0x70's page would eventually
    // splice it onto whatever finishes first.
    const Result<PageAssembler::Feed> fresh = assembler.feed(page(0x71, 0, 0, { 0x11, 0x22 }));

    check(fresh.has_value() && *fresh == PageAssembler::Feed::Complete,
          "a new transmission starting at page 0 is accepted immediately");
    check(collected(assembler) == std::vector<std::uint8_t> { 0x11, 0x22 },
          "and carries none of the abandoned transmission's bytes");
    check(assembler.stats().restarts == 1, "the abandonment is counted");
}

void test_a_continuation_of_a_different_transmission_is_refused()
{
    PageAssembler assembler;

    check(assembler.feed(page(0x80, 0, 1, { 0xAA })).has_value(), "page 0 of transmission 0x80 accepted");

    const Result<PageAssembler::Feed> alien = assembler.feed(page(0x81, 1, 1, { 0xBB }));
    check(!alien.has_value() && alien.error().kind == ErrorKind::PageOutOfOrder,
          "page 1 of a DIFFERENT transmission is not a continuation of this one");
}

void test_a_changed_max_page_index_is_refused()
{
    PageAssembler assembler;

    check(assembler.feed(page(0x90, 0, 2, { 0xAA })).has_value(), "page 0 of 2 accepted");

    const Result<PageAssembler::Feed> inconsistent = assembler.feed(page(0x90, 1, 3, { 0xBB }));
    check(!inconsistent.has_value() && inconsistent.error().kind == ErrorKind::PageOutOfOrder,
          "a page that disagrees about how many pages there are is refused");
}

// ============================================================================
// Bounds
// ============================================================================

void test_payload_is_bounded()
{
    // A receiver that never sends a final page. Without a cap the buffer grows
    // for as long as the process runs.
    PageAssembler assembler(1024);

    Result<PageAssembler::Feed> fed = PageAssembler::Feed::Incomplete;
    for (int index = 0; index < 20 && fed.has_value(); ++index)
    {
        fed = assembler.feed(page(0xA0, static_cast<std::uint8_t>(index), 255,
                                  std::vector<std::uint8_t>(200, 0x5A)));
    }

    check(!fed.has_value() && fed.error().kind == ErrorKind::TooLong,
          "a transmission that never ends is cut off");
    check(assembler.stats().overflows == 1, "and the overflow is counted");

    // And the assembler still works.
    const Result<PageAssembler::Feed> after = assembler.feed(page(0xA1, 0, 0, { 0x01 }));
    check(after.has_value() && *after == PageAssembler::Feed::Complete,
          "a normal transmission is accepted after an overflow");
}

void test_reset_drops_a_partial_transmission()
{
    PageAssembler assembler;

    check(assembler.feed(page(0xB0, 0, 1, { 0xAA })).has_value(), "page 0 accepted");
    assembler.reset();

    // After a reconnect, the continuation of a transmission from before the
    // drop must not be able to attach to anything.
    const Result<PageAssembler::Feed> orphan = assembler.feed(page(0xB0, 1, 1, { 0xBB }));
    check(!orphan.has_value() && orphan.error().kind == ErrorKind::PageOutOfOrder,
          "a continuation page after reset() has nothing to continue");
}

} // namespace

int main()
{
    spdlog::set_level(spdlog::level::info);

    test_single_page_completes_immediately();
    test_a_page_with_no_records_is_legal();
    test_header_shorter_than_three_bytes_is_truncated();
    test_three_pages_reassemble_in_order();
    test_record_split_across_a_page_boundary();
    test_page_count_at_the_protocol_maximum();
    test_a_missing_page_is_reported_not_spliced();
    test_a_duplicated_page_is_refused();
    test_a_new_transmission_number_abandons_the_old_one();
    test_a_continuation_of_a_different_transmission_is_refused();
    test_a_changed_max_page_index_is_refused();
    test_payload_is_bounded();
    test_reset_drops_a_partial_transmission();

    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }

    SPDLOG_INFO("all GSOF page assembly checks passed");
    return 0;
}
