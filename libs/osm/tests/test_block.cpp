// SPDX-License-Identifier: GPL-3.0-or-later
//
// The PBF block grammar.
//
// Every case here is a trap that RENDERS rather than fails. A block whose
// coordinates are off by a factor of a hundred draws a map of the right shape
// in the wrong hemisphere; a keys_vals array read as a list per node gives
// every node its neighbour's name; a delta accumulator that survives a group
// boundary walks the rest of the file into the ocean. None of them throw.

#include <cstdint>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#include "osm/block.h"
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

// Irvine, the anchor this whole map stack is tested against: the same point
// libs/mvt pulls out of the real archive and the same tile docs/map.md names.
constexpr std::int64_t kIrvineLat = 336865966;
constexpr std::int64_t kIrvineLon = -1178557874;

void test_dense_nodes_decode_with_their_coordinates()
{
    osm_test::PrimitiveBlockSpec spec;
    spec.strings = { "", "highway", "traffic_signals" };

    osm_test::GroupSpec group;
    group.dense.push_back({ 1001, kIrvineLat, kIrvineLon, {} });
    group.dense.push_back({ 1002, kIrvineLat + 500, kIrvineLon + 500, { { 1, 2 } } });
    spec.groups.push_back(group);

    auto block = osm::decodeDataBlock(osm_test::primitiveBlock(spec));
    check(block.has_value(), "a block of dense nodes decodes");
    if (!block)
    {
        SPDLOG_ERROR("  {}", osm::to_string(block.error()));
        return;
    }

    check(block->nodes().size() == 2, "with both nodes");
    if (block->nodes().size() != 2)
    {
        return;
    }

    // Ids are delta-coded zigzag; a decoder that skipped the accumulation would
    // report the second id as 1.
    check(block->nodes()[0].id == 1001, "the first id");
    check(block->nodes()[1].id == 1002, "the second id, after accumulating its delta");

    // The default granularity of 100 nanodegrees is exactly the 1e-7 degrees
    // this tree stores, so the value passes through unscaled. Getting the
    // granularity wrong is a factor of 100, which is a different continent.
    check(block->nodes()[0].lat == static_cast<osm::Coord>(kIrvineLat),
          "and the latitude survives the nanodegree conversion");
    check(block->nodes()[0].lon == static_cast<osm::Coord>(kIrvineLon),
          "and so does the longitude, sign included");
    check(block->nodes()[1].lat == static_cast<osm::Coord>(kIrvineLat + 500),
          "with the second node's delta accumulated");
}

void test_keys_vals_is_one_flat_array_terminated_per_node()
{
    // THE trap. keys_vals is a single array with a 0 after each node's pairs,
    // not a list per node. A node with no tags contributes only its terminator,
    // and a reader that assumes otherwise shifts every later node's tags by one
    // -- so every node in the block ends up with its neighbour's name, silently.
    osm_test::PrimitiveBlockSpec spec;
    spec.strings = { "", "name", "First", "Second", "highway", "residential" };

    osm_test::GroupSpec group;
    group.dense.push_back({ 1, 100, 100, { { 1, 2 } } });                  // name=First
    group.dense.push_back({ 2, 200, 200, {} });                            // no tags at all
    group.dense.push_back({ 3, 300, 300, { { 1, 3 }, { 4, 5 } } });        // name=Second, highway
    spec.groups.push_back(group);

    auto block = osm::decodeDataBlock(osm_test::primitiveBlock(spec));
    check(block.has_value(), "a block with mixed tagging decodes");
    if (!block || block->nodes().size() != 3)
    {
        return;
    }

    const auto& nodes = block->nodes();

    auto first = block->value(nodes[0], "name");
    check(first.has_value() && *first == "First", "the first node keeps its own name");

    check(block->tags(nodes[1]).empty(), "the untagged node has no tags");

    auto third = block->value(nodes[2], "name");
    check(third.has_value() && *third == "Second",
          "and the node after it is NOT shifted onto its neighbour's tags");
    auto highway = block->value(nodes[2], "highway");
    check(highway.has_value() && *highway == "residential", "keeping both of its own tags");
}

void test_a_block_with_no_keys_vals_at_all()
{
    // The shape a block of untagged nodes really has: the field is absent
    // entirely rather than present and empty.
    osm_test::PrimitiveBlockSpec spec;
    spec.strings = { "" };
    spec.emitKeysVals = false;

    osm_test::GroupSpec group;
    group.dense.push_back({ 1, 100, 100, {} });
    group.dense.push_back({ 2, 200, 200, {} });
    spec.groups.push_back(group);

    auto block = osm::decodeDataBlock(osm_test::primitiveBlock(spec));
    check(block.has_value(), "a block with no keys_vals field decodes");
    if (!block)
    {
        return;
    }
    check(block->nodes().size() == 2, "with its nodes");
    for (const osm::Node& node : block->nodes())
    {
        check(block->tags(node).empty(), "and none of them has tags");
    }
}

void test_delta_accumulators_reset_at_each_group()
{
    // Deltas accumulate across a GROUP and reset with it. A decoder that
    // carried them across groups would place every node after the first group
    // at roughly twice its coordinate -- which is a real place, and wrong.
    osm_test::PrimitiveBlockSpec spec;
    spec.strings = { "" };

    osm_test::GroupSpec first;
    first.dense.push_back({ 100, 1000, 2000, {} });
    first.dense.push_back({ 101, 1100, 2100, {} });

    osm_test::GroupSpec second;
    second.dense.push_back({ 200, 5000, 6000, {} });

    spec.groups.push_back(first);
    spec.groups.push_back(second);

    auto block = osm::decodeDataBlock(osm_test::primitiveBlock(spec));
    check(block.has_value(), "a two-group block decodes");
    if (!block || block->nodes().size() != 3)
    {
        return;
    }

    check(block->nodes()[2].id == 200, "the second group's id starts from zero again");
    check(block->nodes()[2].lat == 5000,
          "and so does its latitude -- not 6100, which is what carrying the accumulator gives");
    check(block->nodes()[2].lon == 6000, "and its longitude");
}

void test_granularity_and_offset_are_applied()
{
    // A file written with a coarser granularity or a bbox offset is legal and
    // rare, which is exactly why nobody notices when it is handled wrongly.
    osm_test::PrimitiveBlockSpec spec;
    spec.strings = { "" };
    spec.granularity = 1000;    // ten times coarser than the default
    spec.latOffset = 100000;    // nanodegrees

    osm_test::GroupSpec group;
    group.dense.push_back({ 1, 7, 3, {} });
    spec.groups.push_back(group);

    auto block = osm::decodeDataBlock(osm_test::primitiveBlock(spec));
    check(block.has_value(), "a block with a non-default frame decodes");
    if (!block || block->nodes().empty())
    {
        return;
    }

    // lat = (100000 + 1000*7) nanodeg = 107000 nanodeg = 1070 * 1e-7 deg
    check(block->nodes()[0].lat == 1070, "latitude applies both granularity and offset");
    // lon has no offset: 1000*3 = 3000 nanodeg = 30 * 1e-7 deg
    check(block->nodes()[0].lon == 30, "longitude applies granularity alone");
}

void test_way_refs_are_delta_coded()
{
    // Refs are delta-coded; the way id above them is not. A decoder that reads
    // refs as absolute gets node 1, node 2, node 3 -- a real, drawable,
    // entirely wrong shape.
    osm_test::PrimitiveBlockSpec spec;
    spec.strings = { "", "highway", "motorway" };

    osm_test::GroupSpec group;
    group.ways.push_back({ 5000, { 1001, 1002, 1005, 1009 }, { { 1, 2 } } });
    spec.groups.push_back(group);

    auto block = osm::decodeDataBlock(osm_test::primitiveBlock(spec));
    check(block.has_value(), "a block with a way decodes");
    if (!block || block->ways().empty())
    {
        return;
    }

    const osm::Way& way = block->ways()[0];
    check(way.id == 5000, "the way id is not zigzag-halved");

    const auto refs = block->refs(way);
    check(refs.size() == 4, "with all four refs");
    if (refs.size() == 4)
    {
        check(refs[0] == 1001 && refs[1] == 1002 && refs[2] == 1005 && refs[3] == 1009,
              "and they accumulate to the ids that were written");
    }

    auto highway = block->value(way, "highway");
    check(highway.has_value() && *highway == "motorway", "and its tags read");
}

void test_relations_decode_with_roles_and_types()
{
    // Member ids are delta-coded; roles and types beside them are not. The
    // asymmetry is real, and getting it wrong resolves a turn restriction to
    // the wrong way.
    osm_test::PrimitiveBlockSpec spec;
    spec.strings = { "", "type", "restriction", "from", "via", "to" };

    osm_test::GroupSpec group;
    osm_test::RelationSpec relation;
    relation.id = 9000;
    relation.tags = { { 1, 2 } };
    relation.members = {
        { 5000, 1, 3 },  // way 5000, role "from"
        { 1002, 0, 4 },  // node 1002, role "via"
        { 5001, 1, 5 },  // way 5001, role "to"
    };
    group.relations.push_back(relation);
    spec.groups.push_back(group);

    auto block = osm::decodeDataBlock(osm_test::primitiveBlock(spec));
    check(block.has_value(), "a block with a relation decodes");
    if (!block || block->relations().empty())
    {
        return;
    }

    const osm::Relation& r = block->relations()[0];
    check(r.id == 9000, "the relation id");

    auto type = block->value(r, "type");
    check(type.has_value() && *type == "restriction", "and its type tag");

    const auto members = block->members(r);
    check(members.size() == 3, "with all three members");
    if (members.size() != 3)
    {
        return;
    }

    check(members[0].ref == 5000 && members[0].type == osm::MemberType::Way,
          "the from member is a way");
    check(block->string(members[0].roleIndex) == "from", "with the right role");
    check(members[1].ref == 1002 && members[1].type == osm::MemberType::Node,
          "the via member is a node, its id accumulated from a NEGATIVE delta");
    check(block->string(members[1].roleIndex) == "via", "with the right role");
    check(members[2].ref == 5001 && members[2].type == osm::MemberType::Way,
          "and the to member is a way");
    check(block->string(members[2].roleIndex) == "to", "with the right role");
}

void test_a_tag_index_past_the_string_table_is_refused()
{
    // Silently clamping would give the entity a tag belonging to something
    // else, which is worse than no tag at all.
    osm_test::PrimitiveBlockSpec spec;
    spec.strings = { "", "highway" };

    osm_test::GroupSpec group;
    group.dense.push_back({ 1, 100, 100, { { 1, 99 } } });  // value index 99 does not exist
    spec.groups.push_back(group);

    auto block = osm::decodeDataBlock(osm_test::primitiveBlock(spec));
    check(!block.has_value(), "a tag index past the string table is refused");
    if (!block)
    {
        check(block.error().kind == osm::Error::Kind::Malformed, "as Malformed");
    }
}

void test_peeking_finds_what_a_block_holds_without_decoding_it()
{
    // Pass A of the node store reads ways and relations and must step over node
    // blocks as cheaply as possible.
    osm_test::PrimitiveBlockSpec nodesOnly;
    nodesOnly.strings = { "" };
    osm_test::GroupSpec nodeGroup;
    nodeGroup.dense.push_back({ 1, 100, 100, {} });
    nodesOnly.groups.push_back(nodeGroup);

    auto peeked = osm::peekDataBlock(osm_test::primitiveBlock(nodesOnly));
    check(peeked.has_value(), "a node block peeks");
    if (peeked)
    {
        check(peeked->hasNodes, "and says it has nodes");
        check(!peeked->hasWays && !peeked->hasRelations, "and nothing else");
    }

    osm_test::PrimitiveBlockSpec waysOnly;
    waysOnly.strings = { "" };
    osm_test::GroupSpec wayGroup;
    wayGroup.ways.push_back({ 1, { 1, 2 }, {} });
    waysOnly.groups.push_back(wayGroup);

    auto peekedWays = osm::peekDataBlock(osm_test::primitiveBlock(waysOnly));
    check(peekedWays.has_value(), "a way block peeks");
    if (peekedWays)
    {
        check(peekedWays->hasWays, "and says it has ways");
        check(!peekedWays->hasNodes, "and no nodes");
    }
}

void test_header_features_are_read_and_checked()
{
    osm_test::HeaderBlockSpec spec;
    spec.optionalFeatures = { "Sort.Type_then_ID" };
    spec.hasBbox = true;
    // Nanodegrees: -118 degrees is -118e9, not -1.18e9. The zero count is the
    // whole conversion, which is why the assertion below checks the magnitude.
    spec.left = -118'000'000'000;
    spec.right = -117'000'000'000;
    spec.top = 34'000'000'000;
    spec.bottom = 33'000'000'000;
    spec.writingProgram = "osmium/1.0";

    auto header = osm::decodeHeaderBlock(osm_test::headerBlock(spec));
    check(header.has_value(), "a header block decodes");
    if (!header)
    {
        return;
    }

    check(header->requiredFeatures.size() == 2, "with its required features");
    check(header->optionalFeatures.size() == 1 &&
              header->optionalFeatures[0] == "Sort.Type_then_ID",
          "and its optional ones");
    check(header->hasBbox, "and a bbox");
    check(header->west < -117.9 && header->west > -118.1, "whose west edge converts to degrees");
    check(header->writingProgram == "osmium/1.0", "and the writing program");

    check(osm::checkRequiredFeatures(*header).has_value(),
          "and a file requiring only what we implement is accepted");
}

void test_a_full_history_file_is_refused_by_name()
{
    // The node store holds one version per id. A full-history file carries
    // several, so the last one silently wins and some of the geometry comes
    // from whenever that edit happened. Refused, and the message says why.
    osm_test::HeaderBlockSpec spec;
    spec.requiredFeatures = { "OsmSchema-V0.6", "DenseNodes", "HistoricalInformation" };

    auto header = osm::decodeHeaderBlock(osm_test::headerBlock(spec));
    check(header.has_value(), "the header still decodes");
    if (!header)
    {
        return;
    }

    auto ok = osm::checkRequiredFeatures(*header);
    check(!ok.has_value(), "but a full-history file is refused");
    if (!ok)
    {
        check(ok.error().kind == osm::Error::Kind::Unsupported, "as Unsupported");
        check(ok.error().message.find("history") != std::string::npos,
              "and the message says which feature");
    }
}

void test_an_unknown_required_feature_is_refused()
{
    // required_features exists precisely so a reader that would misinterpret
    // the file stops instead of guessing.
    osm_test::HeaderBlockSpec spec;
    spec.requiredFeatures = { "OsmSchema-V0.6", "SomeFutureThing" };

    auto header = osm::decodeHeaderBlock(osm_test::headerBlock(spec));
    if (!header)
    {
        check(false, "header decodes");
        return;
    }

    auto ok = osm::checkRequiredFeatures(*header);
    check(!ok.has_value(), "an unknown required feature is refused");
    if (!ok)
    {
        check(ok.error().message.find("SomeFutureThing") != std::string::npos,
              "and named in the message");
    }
}

} // namespace

int main()
{
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%^%l%$] %v");

    test_dense_nodes_decode_with_their_coordinates();
    test_keys_vals_is_one_flat_array_terminated_per_node();
    test_a_block_with_no_keys_vals_at_all();
    test_delta_accumulators_reset_at_each_group();
    test_granularity_and_offset_are_applied();
    test_way_refs_are_delta_coded();
    test_relations_decode_with_roles_and_types();
    test_a_tag_index_past_the_string_table_is_refused();
    test_peeking_finds_what_a_block_holds_without_decoding_it();
    test_header_features_are_read_and_checked();
    test_a_full_history_file_is_refused_by_name();
    test_an_unknown_required_feature_is_refused();

    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }

    SPDLOG_INFO("all PBF block checks passed");
    return 0;
}
