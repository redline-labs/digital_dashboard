// SPDX-License-Identifier: GPL-3.0-or-later
//
// The bus-topic CanFrame to helpers::CanFrame conversion, and back.
//
// What is pinned is the length rule, because every CAN consumer used to copy
// the payload by hand and the length is the part that goes wrong quietly: a
// publisher that declares more bytes than it supplies, or supplies more than it
// declares, has to come out as the bytes that really exist. A decoder handed a
// padded buffer reads the padding as readings.

#include "pub_sub/can_frame.h"

#include <capnp/message.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
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

// One message, built the way a publisher would: `len` and the list set
// independently, so the two can disagree.
helpers::CanFrame decode(std::uint8_t len, const std::vector<std::uint8_t>& payload)
{
    capnp::MallocMessageBuilder message;
    auto builder = message.initRoot<::CanFrame>();
    builder.setId(0x18FF50E5u);
    builder.setExtended(true);
    builder.setLen(len);
    builder.setData(kj::arrayPtr(payload.data(), payload.size()));
    return pub_sub::fromCapnp(builder.asReader());
}

void testLengthIsWhatWasSupplied()
{
    const std::vector<std::uint8_t> eight{1, 2, 3, 4, 5, 6, 7, 8};

    const auto exact = decode(8u, eight);
    expect(exact.len == 8u, "declared and supplied agree");
    expect(exact.data_span().back() == 8u, "last byte copied");

    const auto claimsMore = decode(12u, eight);
    expect(claimsMore.len == 8u, "a len past the list stops at the list");

    const auto claimsLess = decode(3u, eight);
    expect(claimsLess.len == 3u, "a list past len stops at len");
    expect(claimsLess.data[3] == 0u, "nothing past len is copied");

    const std::vector<std::uint8_t> oversized(100u, 0xAAu);
    const auto huge = decode(255u, oversized);
    expect(huge.len == 64u, "never more than a frame holds");
}

void testFlagsSurvive()
{
    const auto frame = decode(0u, {});
    expect(frame.id == 0x18FF50E5u, "id");
    expect(frame.isExtended, "extended flag");
    expect(!frame.isFD && !frame.isRTR && !frame.isError, "unset flags stay unset");
    expect(frame.len == 0u, "empty payload");
}

void testRoundTrip()
{
    helpers::CanFrame original;
    original.id = 0x123u;
    original.isFD = true;
    original.isBRS = true;
    original.isESI = true;
    original.timestampUs = 1'234'567u;
    original.len = 12u;
    for (std::size_t i = 0u; i < original.len; ++i)
    {
        original.data[i] = static_cast<std::uint8_t>(0xF0u + i);
    }

    capnp::MallocMessageBuilder message;
    auto builder = message.initRoot<::CanFrame>();
    pub_sub::toCapnp(original, builder);
    expect(builder.asReader().getData().size() == 12u, "the list is len long");

    const auto back = pub_sub::fromCapnp(builder.asReader());
    expect(back.id == original.id, "id round trips");
    expect(back.isFD && back.isBRS && back.isESI, "FD flags round trip");
    expect(back.timestampUs == original.timestampUs, "timestamp round trips");
    expect(back.len == 12u, "len round trips");
    expect(back.data == original.data, "payload round trips");
}

}  // namespace

int main()
{
    testLengthIsWhatWasSupplied();
    testFlagsSurvive();
    testRoundTrip();

    std::fprintf(stderr, "%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
