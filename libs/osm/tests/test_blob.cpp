// SPDX-License-Identifier: GPL-3.0-or-later
//
// PBF framing.
//
// The failures here are the ones that let a reader keep going: a length prefix
// read the wrong way round parses the first blob and turns everything after it
// into garbage, a size cap that is not enforced turns a corrupt four bytes into
// a four-gigabyte read, and a compression codec that is silently ignored yields
// an EMPTY block rather than an error -- which reads as a file with no data in
// it rather than as a file this build cannot read.

#include <cstdint>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#include "osm/blob.h"
#include "pbf_builder.h"

namespace
{

using osm_test::Bytes;

int failures = 0;

void check(bool condition, const std::string& what)
{
    if (!condition)
    {
        SPDLOG_ERROR("FAIL: {}", what);
        ++failures;
    }
}

Bytes headerBytes()
{
    osm_test::HeaderBlockSpec spec;
    return osm_test::headerBlock(spec);
}

void test_a_file_of_framed_blobs_walks()
{
    Bytes file = osm_test::framed("OSMHeader", osm_test::rawBlob(headerBytes()));
    const Bytes data = osm_test::framed("OSMData", osm_test::rawBlob(Bytes { 0x08, 0x01 }));
    file.insert(file.end(), data.begin(), data.end());

    osm::BlobIterator it(file);

    auto first = it.next();
    check(first.has_value(), "the first blob reads");
    if (first)
    {
        check(first->kind == osm::BlobKind::Header, "and is the header");
        check(first->offset == 0, "at offset 0");
    }

    auto second = it.next();
    check(second.has_value(), "the second blob reads");
    if (second)
    {
        check(second->kind == osm::BlobKind::Data, "and is data");
        check(second->offset > 0, "at a later offset");
    }

    check(it.done(), "and the file is then exhausted");
}

void test_the_length_prefix_is_big_endian()
{
    // The only big-endian number in the format. Read little-endian, a header of
    // 13 bytes becomes 218103808 -- which the size cap then rejects, so this
    // asserts the cap fires rather than that the read succeeds by accident.
    Bytes file = osm_test::framed("OSMHeader", osm_test::rawBlob(headerBytes()));

    // Byte-swap the prefix in place and confirm it is no longer readable.
    std::swap(file[0], file[3]);
    std::swap(file[1], file[2]);

    osm::BlobIterator it(file);
    auto blob = it.next();
    check(!blob.has_value(), "a byte-swapped length prefix is refused");
}

void test_an_absurd_header_length_is_capped()
{
    const Bytes file { 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
    osm::BlobIterator it(file);
    auto blob = it.next();
    check(!blob.has_value(), "a 4 GB blob header is refused rather than read");
    if (!blob)
    {
        check(blob.error().kind == osm::Error::Kind::Malformed, "as Malformed");
    }
}

void test_a_truncated_file_is_refused()
{
    Bytes file = osm_test::framed("OSMData", osm_test::rawBlob(Bytes { 0x08, 0x01 }));
    file.resize(file.size() - 1);

    osm::BlobIterator it(file);
    auto blob = it.next();
    check(!blob.has_value(), "a file cut mid-blob is refused");
    if (!blob)
    {
        check(blob.error().kind == osm::Error::Kind::Truncated, "as Truncated");
    }
}

void test_a_raw_blob_round_trips()
{
    const Bytes payload { 0x08, 0x2A, 0x10, 0x07 };
    const Bytes file = osm::BlobIterator(Bytes {}).done()
                           ? osm_test::framed("OSMData", osm_test::rawBlob(payload))
                           : Bytes {};

    osm::BlobIterator it(file);
    auto blob = it.next();
    check(blob.has_value(), "an uncompressed blob frames");
    if (!blob)
    {
        return;
    }

    std::vector<std::uint8_t> out;
    auto ok = osm::inflateBlob(*blob, out);
    check(ok.has_value(), "and unwraps");
    check(out == payload, "to exactly its payload");
}

void test_a_zlib_blob_round_trips()
{
    Bytes payload;
    // Something long enough that compression actually engages.
    for (int i = 0; i < 500; ++i)
    {
        payload.push_back(static_cast<std::uint8_t>(i % 7));
    }

    const Bytes file = osm_test::framed("OSMData", osm_test::zlibBlob(payload));

    osm::BlobIterator it(file);
    auto blob = it.next();
    check(blob.has_value(), "a zlib blob frames");
    if (!blob)
    {
        return;
    }

    std::vector<std::uint8_t> out;
    auto ok = osm::inflateBlob(*blob, out);
    check(ok.has_value(), "and inflates");
    check(out == payload, "to exactly its payload");
}

void test_the_buffer_is_reused_rather_than_reallocated()
{
    // The whole reason inflateBlob takes the buffer by reference: a continental
    // file is over a million blocks, and a fresh vector per block is a million
    // allocations of the largest size class in the program.
    Bytes payload(1000, 0x5A);
    const Bytes file = osm_test::framed("OSMData", osm_test::zlibBlob(payload));

    std::vector<std::uint8_t> out;
    out.reserve(4096);
    const void* before = out.data();

    for (int i = 0; i < 5; ++i)
    {
        osm::BlobIterator it(file);
        auto blob = it.next();
        if (!blob)
        {
            check(false, "blob frames");
            return;
        }
        auto ok = osm::inflateBlob(*blob, out);
        check(ok.has_value(), "each inflate succeeds");
    }

    check(out.data() == before, "and the caller's buffer is never reallocated");
}

void test_an_unimplemented_codec_is_named_rather_than_ignored()
{
    // A blob compressed with lzma/zstd must be an error naming the codec. The
    // failure mode this guards against is returning an empty block, which reads
    // as "this area has no data" rather than "this build cannot read this file".
    Bytes blob;
    // Blob.raw_size, then Blob.zstd_data (field 7).
    osm_test::putVarintField(blob, 2, 100);
    osm_test::putLengthDelimited(blob, 7, Bytes { 0x28, 0xB5, 0x2F, 0xFD });

    const Bytes file = osm_test::framed("OSMData", blob);

    osm::BlobIterator it(file);
    auto framedBlob = it.next();
    check(framedBlob.has_value(), "the blob still frames");
    if (!framedBlob)
    {
        return;
    }

    std::vector<std::uint8_t> out;
    auto ok = osm::inflateBlob(*framedBlob, out);
    check(!ok.has_value(), "a zstd blob is refused");
    if (!ok)
    {
        check(ok.error().kind == osm::Error::Kind::Unsupported, "as Unsupported");
        check(ok.error().message.find("zstd") != std::string::npos,
              "and the message names the codec");
    }
}

void test_a_blob_that_lies_about_its_size_is_refused()
{
    // raw_size is what sizes the buffer AND what the block's own field offsets
    // are read against, so a disagreement is not cosmetic.
    Bytes payload(200, 0x11);
    Bytes blob;
    osm_test::putVarintField(blob, 2, 999);  // claims 999, will inflate to 200
    osm_test::putLengthDelimited(blob, 3, osm_test::deflateBytes(payload));

    const Bytes file = osm_test::framed("OSMData", blob);
    osm::BlobIterator it(file);
    auto framedBlob = it.next();
    if (!framedBlob)
    {
        check(false, "blob frames");
        return;
    }

    std::vector<std::uint8_t> out;
    auto ok = osm::inflateBlob(*framedBlob, out);
    check(!ok.has_value(), "a blob whose raw_size disagrees with reality is refused");
}

void test_an_unknown_block_type_is_skippable_rather_than_fatal()
{
    // The format reserves the right to add block types, and a reader that
    // refused them would break on the next revision. It must still be framed
    // and stepped over correctly.
    Bytes file = osm_test::framed("OSMSomethingNew", osm_test::rawBlob(Bytes { 0x08, 0x01 }));
    const Bytes data = osm_test::framed("OSMData", osm_test::rawBlob(Bytes { 0x08, 0x02 }));
    file.insert(file.end(), data.begin(), data.end());

    osm::BlobIterator it(file);
    auto first = it.next();
    check(first.has_value() && first->kind == osm::BlobKind::Unknown,
          "an unrecognised block type frames as Unknown");

    auto second = it.next();
    check(second.has_value() && second->kind == osm::BlobKind::Data,
          "and the data block after it is still found");
}

} // namespace

int main()
{
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%^%l%$] %v");

    test_a_file_of_framed_blobs_walks();
    test_the_length_prefix_is_big_endian();
    test_an_absurd_header_length_is_capped();
    test_a_truncated_file_is_refused();
    test_a_raw_blob_round_trips();
    test_a_zlib_blob_round_trips();
    test_the_buffer_is_reused_rather_than_reallocated();
    test_an_unimplemented_codec_is_named_rather_than_ignored();
    test_a_blob_that_lies_about_its_size_is_refused();
    test_an_unknown_block_type_is_skippable_rather_than_fatal();

    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }

    SPDLOG_INFO("all PBF framing checks passed");
    return 0;
}
