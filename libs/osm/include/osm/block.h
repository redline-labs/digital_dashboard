// SPDX-License-Identifier: GPL-3.0-or-later
//
// Decoding one PBF block.
//
// decodeDataBlock() is a PURE FUNCTION of the inflated bytes -- it touches no
// shared state, so a caller can run one per core with no coordination. That is
// the whole reason blob.h keeps framing separate from this.
//
// Five things about the format are easy to get subtly wrong, and every one of
// them produces plausible data rather than a failure:
//
//   | Trap                                     | What it looks like            |
//   |------------------------------------------|-------------------------------|
//   | coords are 1e-9*(offset+granularity*d)   | everything off by 100x        |
//   | keys_vals is ONE array, 0-terminated     | every node gets its           |
//   |   per node, not a list per node          |   neighbour's name            |
//   | delta accumulators reset per GROUP,      | nodes drift into the ocean    |
//   |   not per block                          |   partway through a file      |
//   | ids and deltas are zigzag (sint64)       | refs resolve to nothing       |
//   | way refs are delta-coded too             | every way collapses to a point|
//
// osm_test_block covers each.
#ifndef OSM_BLOCK_H
#define OSM_BLOCK_H

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "osm/entity.h"
#include "osm/error.h"

namespace osm
{

// What the OSMHeader block says about the file.
struct Header
{
    // Strings a reader MUST understand to read this file correctly. An unknown
    // one is refused: that is what the field is for, and guessing is how you
    // silently misread a format revision.
    std::vector<std::string> requiredFeatures;
    // Advisory. `Sort.Type_then_ID` lives here, and it is NOT trusted -- see
    // node_store.h for why the ordering is verified while streaming instead.
    std::vector<std::string> optionalFeatures;

    bool hasBbox { false };
    double west { 0.0 };
    double south { 0.0 };
    double east { 0.0 };
    double north { 0.0 };

    std::string writingProgram;
    std::int64_t replicationTimestamp { 0 };
};

Result<Header> decodeHeaderBlock(std::span<const std::uint8_t> bytes);

// Refuse a file this build cannot read correctly.
//
// Separate from decodeHeaderBlock so a tool can report everything it found
// before deciding, and so the test can drive the decision without a file.
Result<void> checkRequiredFeatures(const Header& header);

// One OSMData block, decoded.
//
// The arrays are flat and shared; entities hold ranges into them. Everything is
// owned by the block, so it can outlive the buffer it was decoded from.
class Block
{
  public:
    const std::vector<std::string>& strings() const { return mStrings; }
    const std::vector<Node>& nodes() const { return mNodes; }
    const std::vector<Way>& ways() const { return mWays; }
    const std::vector<Relation>& relations() const { return mRelations; }

    std::string_view string(std::uint32_t index) const
    {
        return index < mStrings.size() ? std::string_view(mStrings[index]) : std::string_view();
    }

    std::span<const Tag> tags(const Node& n) const { return tagSpan(n.tagBegin, n.tagCount); }
    std::span<const Tag> tags(const Way& w) const { return tagSpan(w.tagBegin, w.tagCount); }
    std::span<const Tag> tags(const Relation& r) const { return tagSpan(r.tagBegin, r.tagCount); }

    std::span<const std::int64_t> refs(const Way& w) const
    {
        return std::span<const std::int64_t>(mRefs).subspan(w.refBegin, w.refCount);
    }

    std::span<const Member> members(const Relation& r) const
    {
        return std::span<const Member>(mMembers).subspan(r.memberBegin, r.memberCount);
    }

    // The value of a tag, by key name. Absent when the entity has no such tag.
    template <typename EntityT>
    std::optional<std::string_view> value(const EntityT& entity, std::string_view key) const
    {
        for (const Tag& tag : tags(entity))
        {
            if (string(tag.key) == key)
            {
                return string(tag.value);
            }
        }
        return std::nullopt;
    }

    // Populated by decodeDataBlock; public only so the decoder can fill them.
    std::vector<std::string> mStrings;
    std::vector<Tag> mTags;
    std::vector<std::int64_t> mRefs;
    std::vector<Member> mMembers;
    std::vector<Node> mNodes;
    std::vector<Way> mWays;
    std::vector<Relation> mRelations;

  private:
    std::span<const Tag> tagSpan(std::uint32_t begin, std::uint32_t count) const
    {
        return std::span<const Tag>(mTags).subspan(begin, count);
    }
};

// Decode one inflated OSMData block. Pure; safe to call concurrently.
//
// `blockOffset` is only used to rebase error offsets onto the file, so an error
// names a byte a person can find rather than one inside a buffer that no longer
// exists.
Result<Block> decodeDataBlock(std::span<const std::uint8_t> bytes, std::size_t blockOffset = 0);

// What kinds of entity a block holds, without decoding it fully.
//
// Pass A of the node store reads ways and relations and must skip node blocks
// as cheaply as possible; this parses group headers only. On a file where nodes
// and ways live in separate blocks -- which is every sorted file -- it lets the
// first pass skip most of the data.
struct BlockContents
{
    bool hasNodes { false };
    bool hasWays { false };
    bool hasRelations { false };
};

Result<BlockContents> peekDataBlock(std::span<const std::uint8_t> bytes,
                                    std::size_t blockOffset = 0);

} // namespace osm

#endif // OSM_BLOCK_H
