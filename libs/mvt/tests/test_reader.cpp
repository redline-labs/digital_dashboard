// SPDX-License-Identifier: GPL-3.0-or-later
//
// The protobuf reader underneath the tile decoder.
//
// Split from test_decode.cpp because the failures are different in kind. A tile
// that decodes wrongly renders wrongly; a varint that decodes wrongly produces
// a NUMBER that is wrong, and a number is what every length, index and
// coordinate in the format is made of. The cases below are the ones where a
// plausible implementation silently produces a plausible wrong answer:
// a varint that runs past 64 bits and wraps, a zigzag written as division, a
// length that overruns the buffer.

#include "mvt/reader.h"

#include <spdlog/spdlog.h>

#include <cstdint>
#include <limits>
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

using Bytes = std::vector<std::uint8_t>;

// ============================================================================
// Varints
// ============================================================================

void test_varints_decode()
{
    // Hand-encoded, from the protobuf spec's own worked examples.
    const struct
    {
        Bytes bytes;
        std::uint64_t value;
        const char* what;
    } cases[] = {
        { { 0x00 }, 0, "zero" },
        { { 0x01 }, 1, "one" },
        { { 0x7F }, 127, "the largest single-byte varint" },
        { { 0x80, 0x01 }, 128, "the smallest two-byte varint" },
        { { 0xAC, 0x02 }, 300, "300, the spec's worked example" },
        { { 0xFF, 0xFF, 0xFF, 0xFF, 0x0F }, 0xFFFFFFFFULL, "the largest uint32" },
        { { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01 },
          std::numeric_limits<std::uint64_t>::max(), "the largest uint64, in ten bytes" },
    };

    for (const auto& c : cases)
    {
        mvt::Reader reader(c.bytes);
        auto value = reader.varint();
        check(value.has_value() && *value == c.value, std::string("varint: ") + c.what);
        check(reader.done(), std::string("varint consumes exactly its bytes: ") + c.what);
    }
}

void test_an_overlong_varint_is_refused_rather_than_wrapped()
{
    // Eleven continuation bytes. Shifting the eleventh group by 70 is undefined
    // and, in practice, wraps -- so a decoder without this check answers with a
    // number that is merely wrong.
    const Bytes overlong { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                           0xFF, 0xFF, 0xFF, 0xFF, 0x01 };
    mvt::Reader reader(overlong);
    check(!reader.varint().has_value(), "a varint longer than ten bytes is refused");
}

void test_a_truncated_varint_is_refused()
{
    // Continuation bit set on the last byte available.
    const Bytes cut { 0x80 };
    mvt::Reader reader(cut);
    auto value = reader.varint();
    check(!value.has_value(), "a varint whose continuation runs off the end is refused");
    if (!value)
    {
        check(value.error().kind == mvt::Error::Kind::Truncated, "and reports Truncated");
    }
}

// ============================================================================
// Zigzag
// ============================================================================

void test_zigzag_decodes_negatives()
{
    // The mapping from the spec. Written out as a table because the obvious
    // wrong implementation -- halve and negate the odd ones -- agrees with the
    // right one for 0 and 2 and disagrees everywhere else.
    const struct
    {
        std::uint64_t encoded;
        std::int64_t decoded;
    } cases[] = {
        { 0, 0 }, { 1, -1 }, { 2, 1 }, { 3, -2 }, { 4, 2 },
        { 4294967294ULL, 2147483647LL }, { 4294967295ULL, -2147483648LL },
    };

    for (const auto& c : cases)
    {
        check(mvt::unzigzag(c.encoded) == c.decoded,
              "unzigzag(" + std::to_string(c.encoded) + ") == " + std::to_string(c.decoded));
    }

    // Round trip through the reader, which is the path the geometry uses.
    const Bytes minusOne { 0x01 };
    mvt::Reader reader(minusOne);
    auto value = reader.zigzag();
    check(value.has_value() && *value == -1, "the reader decodes a negative zigzag");
}

// ============================================================================
// Fields and framing
// ============================================================================

void test_field_tags_split_into_number_and_wire_type()
{
    // (field 3, wire 2) is 0x1A -- the tag on every layer in every tile.
    const Bytes tag { 0x1A };
    mvt::Reader reader(tag);
    auto field = reader.field();
    check(field.has_value(), "a field tag reads");
    if (field)
    {
        check(field->number == 3, "field number 3");
        check(field->wire == mvt::WireType::LengthDelimited, "wire type 2");
    }
}

void test_field_number_zero_is_refused()
{
    // A run of zero bytes decodes as field 0, wire type 0, forever. This is
    // what stops padding or a zeroed buffer reading as an endless stream of
    // empty fields rather than as the malformed input it is.
    const Bytes zeros(16, 0x00);
    mvt::Reader reader(zeros);
    check(!reader.field().has_value(), "field number 0 is refused");
}

void test_an_invalid_wire_type_is_refused()
{
    // Wire types 6 and 7 have never been assigned.
    const Bytes tag { 0x0F };  // field 1, wire 7
    mvt::Reader reader(tag);
    check(!reader.field().has_value(), "wire type 7 is refused");
}

void test_a_length_that_overruns_the_buffer_is_refused()
{
    // The single most dangerous field in the format: an attacker-controlled
    // length that a decoder trusts is an out-of-bounds read of exactly that
    // many bytes.
    const Bytes claimsTen { 0x0A, 0x01, 0x02 };  // length 10, three bytes present
    mvt::Reader reader(claimsTen);
    auto bytes = reader.bytes();
    check(!bytes.has_value(), "a length past the end of the buffer is refused");
    if (!bytes)
    {
        check(bytes.error().kind == mvt::Error::Kind::Truncated, "and reports Truncated");
    }
}

void test_length_delimited_fields_borrow_the_right_bytes()
{
    const Bytes framed { 0x03, 'a', 'b', 'c', 0x7F };
    mvt::Reader reader(framed);

    auto text = reader.text();
    check(text.has_value() && *text == "abc", "a length-delimited field reads its own bytes");
    check(!reader.done(), "and stops at its end rather than consuming the rest");

    auto after = reader.varint();
    check(after.has_value() && *after == 127, "leaving the next field readable");
}

void test_skipping_advances_by_the_right_amount()
{
    // Skipping is what makes unknown fields survivable, and a skip that
    // advances by the wrong amount desynchronises everything after it -- which
    // presents as a corrupt tile rather than as a skipping bug.
    Bytes stream;
    // field 1, varint 300
    stream.insert(stream.end(), { 0x08, 0xAC, 0x02 });
    // field 2, length-delimited "hi"
    stream.insert(stream.end(), { 0x12, 0x02, 'h', 'i' });
    // field 3, fixed32
    stream.insert(stream.end(), { 0x1D, 0x01, 0x02, 0x03, 0x04 });
    // field 4, fixed64
    stream.insert(stream.end(), { 0x21, 1, 2, 3, 4, 5, 6, 7, 8 });
    // field 5, varint 1 -- the one we must still be able to read
    stream.insert(stream.end(), { 0x28, 0x01 });

    mvt::Reader reader(stream);
    for (int i = 0; i < 4; ++i)
    {
        auto field = reader.field();
        check(field.has_value(), "field " + std::to_string(i + 1) + " reads");
        if (!field)
        {
            return;
        }
        check(reader.skip(field->wire).has_value(),
              "field " + std::to_string(i + 1) + " skips");
    }

    auto last = reader.field();
    check(last.has_value() && last->number == 5,
          "after four skips of four different wire types, field 5 is where it should be");
    if (last)
    {
        auto value = reader.varint();
        check(value.has_value() && *value == 1, "and reads its value");
        check(reader.done(), "with nothing left over");
    }
}

void test_groups_are_refused_rather_than_mis_skipped()
{
    // Deprecated in proto2 and absent from MVT. Skipping one properly means
    // matching it to its end tag; pretending it is skippable would desync the
    // stream silently, so it is refused instead.
    const Bytes group { 0x0B };  // field 1, wire 3 (StartGroup)
    mvt::Reader reader(group);
    auto field = reader.field();
    check(field.has_value(), "a group tag parses as a field");
    if (field)
    {
        auto skipped = reader.skip(field->wire);
        check(!skipped.has_value(), "and skipping it is refused");
        if (!skipped)
        {
            check(skipped.error().kind == mvt::Error::Kind::Unsupported, "as Unsupported");
        }
    }
}

void test_fixed_width_fields_are_little_endian()
{
    const Bytes le32 { 0x01, 0x02, 0x03, 0x04 };
    mvt::Reader r32(le32);
    auto v32 = r32.fixed32();
    check(v32.has_value() && *v32 == 0x04030201U, "fixed32 is little endian");

    const Bytes le64 { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08 };
    mvt::Reader r64(le64);
    auto v64 = r64.fixed64();
    check(v64.has_value() && *v64 == 0x0807060504030201ULL, "fixed64 is little endian");

    const Bytes shortBuffer { 0x01, 0x02 };
    mvt::Reader rs(shortBuffer);
    check(!rs.fixed32().has_value(), "a fixed32 past the end is refused");
}

} // namespace

int main()
{
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%^%l%$] %v");

    test_varints_decode();
    test_an_overlong_varint_is_refused_rather_than_wrapped();
    test_a_truncated_varint_is_refused();

    test_zigzag_decodes_negatives();

    test_field_tags_split_into_number_and_wire_type();
    test_field_number_zero_is_refused();
    test_an_invalid_wire_type_is_refused();
    test_a_length_that_overruns_the_buffer_is_refused();
    test_length_delimited_fields_borrow_the_right_bytes();
    test_skipping_advances_by_the_right_amount();
    test_groups_are_refused_rather_than_mis_skipped();
    test_fixed_width_fields_are_little_endian();

    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }

    SPDLOG_INFO("all protobuf reader checks passed");
    return 0;
}
