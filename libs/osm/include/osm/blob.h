// SPDX-License-Identifier: GPL-3.0-or-later
//
// The outer layer of a PBF: framing, and getting a block's bytes out.
//
// DELIBERATELY SPLIT FROM DECODING. Walking the framing is strictly sequential
// and cheap; inflating and parsing a block is expensive and depends on nothing
// outside that block. Keeping them apart is what lets a caller write
//
//     while (auto blob = it.next()) { pool.submit([blob]{ decodeDataBlock(...); }); }
//
// and get every core working. A continental extract is ~14 GB compressed and
// single-threaded zlib runs at a few hundred MB/s, so this is the difference
// between a five-minute pass and a twenty-minute one -- per pass, and there are
// two. Fusing them into `for (block : file) visitor.onNode(...)` bakes in
// single-threaded, and the fix is a rewrite rather than a parameter.
#ifndef OSM_BLOB_H
#define OSM_BLOB_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "osm/error.h"

namespace osm
{

// The spec's caps. A corrupt four-byte length prefix reading 0xFFFFFFFF is
// otherwise an instant 4 GB read, and the header cap is what stops a garbage
// prefix being interpreted as a plausible one.
inline constexpr std::uint32_t kMaxBlobHeaderBytes = 64 * 1024;
inline constexpr std::uint32_t kMaxBlobBytes = 32 * 1024 * 1024;

enum class BlobKind
{
    // "OSMHeader" -- exactly one, first.
    Header,
    // "OSMData" -- everything else.
    Data,
    // A type string this build does not know. Skippable rather than fatal: the
    // format reserves the right to add block types, and a reader that refused
    // them would break on the next revision.
    Unknown,
};

// One framed blob, still compressed.
struct Blob
{
    BlobKind kind { BlobKind::Unknown };
    std::string type;
    // The Blob message's bytes -- NOT the inflated block. Borrowed from the
    // file buffer and valid as long as it is.
    std::span<const std::uint8_t> message;
    // Where this blob's header starts, for error messages and for logging which
    // block of a very large file went wrong.
    std::size_t offset { 0 };
};

// Walks blob framing without inflating anything.
class BlobIterator
{
  public:
    explicit BlobIterator(std::span<const std::uint8_t> file) : mFile(file) {}

    bool done() const { return mAt >= mFile.size(); }
    std::size_t offset() const { return mAt; }

    // The next blob, or an error. Only call when !done().
    Result<Blob> next();

  private:
    std::span<const std::uint8_t> mFile;
    std::size_t mAt { 0 };
};

// Unwrap a Blob message into `out`, inflating if it is compressed.
//
// `out` is the caller's buffer and is REUSED across calls -- a continental
// extract is over a million blocks of up to 32 MB, and allocating a fresh
// vector per block is a million allocations of the largest size class in the
// program. The Blob carries its own uncompressed size, so the buffer is sized
// exactly rather than grown.
Result<void> inflateBlob(const Blob& blob, std::vector<std::uint8_t>& out);

} // namespace osm

#endif // OSM_BLOB_H
