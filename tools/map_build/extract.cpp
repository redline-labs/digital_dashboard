// SPDX-License-Identifier: GPL-3.0-or-later
#include "map_build/extract.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

#include "osm/blob.h"
#include "osm/block.h"
#include "map_build/rings.h"
#include "map_rules/labels.h"
#include "osm/node_store.h"

namespace map_build
{
namespace
{

class Mapped
{
  public:
    explicit Mapped(const std::filesystem::path& path)
    {
        mFd = ::open(path.c_str(), O_RDONLY);
        if (mFd < 0)
        {
            return;
        }
        struct stat info {};
        if (::fstat(mFd, &info) != 0)
        {
            return;
        }
        mSize = static_cast<std::size_t>(info.st_size);
        void* address = ::mmap(nullptr, mSize, PROT_READ, MAP_PRIVATE, mFd, 0);
        if (address == MAP_FAILED)
        {
            mSize = 0;
            return;
        }
        mData = static_cast<const std::uint8_t*>(address);
    }

    ~Mapped()
    {
        if (mData != nullptr)
        {
            ::munmap(const_cast<std::uint8_t*>(mData), mSize);
        }
        if (mFd >= 0)
        {
            ::close(mFd);
        }
    }

    Mapped(const Mapped&) = delete;
    Mapped& operator=(const Mapped&) = delete;

    bool valid() const { return mData != nullptr; }
    std::span<const std::uint8_t> bytes() const { return { mData, mSize }; }

  private:
    int mFd { -1 };
    const std::uint8_t* mData { nullptr };
    std::size_t mSize { 0 };
};

// Tag pairs for one entity, reused across entities so classify() costs no
// allocations in a loop that runs nine million times.
class TagScratch
{
  public:
    template <typename EntityT>
    map_rules::TagView view(const osm::Block& block, const EntityT& entity)
    {
        mPairs.clear();
        for (const osm::Tag& tag : block.tags(entity))
        {
            mPairs.emplace_back(block.string(tag.key), block.string(tag.value));
        }
        return map_rules::TagView { mPairs };
    }

  private:
    std::vector<std::pair<std::string_view, std::string_view>> mPairs;
};

// A file-wide bbox, grown as coordinates are seen.
struct Bounds
{
    std::int32_t west { std::numeric_limits<std::int32_t>::max() };
    std::int32_t south { std::numeric_limits<std::int32_t>::max() };
    std::int32_t east { std::numeric_limits<std::int32_t>::min() };
    std::int32_t north { std::numeric_limits<std::int32_t>::min() };

    void grow(osm::Coord lat, osm::Coord lon)
    {
        west = std::min(west, lon);
        east = std::max(east, lon);
        south = std::min(south, lat);
        north = std::max(north, lat);
    }

    bool valid() const { return west <= east && south <= north; }

    // Within a tenth of a degree of the edge -- roughly 11 km, comfortably more
    // than the longest way that could straddle a cut.
    bool nearEdge(osm::Coord lat, osm::Coord lon) const
    {
        constexpr osm::Coord kMargin = 1'000'000;
        return lon - west < kMargin || east - lon < kMargin || lat - south < kMargin ||
               north - lat < kMargin;
    }
};

// WHERE TO PUT A LABEL for a shape.
//
// The area-weighted centroid, which is the cheap answer and is right for the
// overwhelming majority of features -- a building, a park, a pond. It is WRONG
// for a strongly concave shape: the centroid of a crescent-shaped reservoir
// falls on the land inside the crescent, and the label sits outside its own
// lake.
//
// The correct answer is the pole of inaccessibility (the point furthest from
// any edge), which is an iterative grid search costing far more than this and
// mattering for a few hundred features out of a hundred thousand. Worth doing
// when something renders these labels and the misplacement is visible; not
// worth doing before then.
//
// Degenerate rings -- zero area, which a collapsed or self-cancelling ring
// gives -- fall back to the vertex average rather than dividing by zero.
std::pair<osm::Coord, osm::Coord> labelPoint(const std::vector<osm::Coord>& ring, bool closed)
{
    if (ring.size() < 2)
    {
        return { 0, 0 };
    }
    if (!closed || ring.size() < 6)
    {
        // A LINE, so the label goes at the middle VERTEX rather than the middle
        // of the bounding box: a river's box centre can be nowhere near the
        // river.
        const std::size_t middle = (ring.size() / 4) * 2;
        return { ring[middle], ring[middle + 1] };
    }

    // Doubled signed area and the first moments, in one pass. Coordinates are
    // 1e-7 degrees and a large ring overflows 32 bits several times over, so
    // every accumulator is double.
    double twiceArea = 0.0;
    double cx = 0.0;
    double cy = 0.0;
    for (std::size_t i = 0; i + 1 < ring.size(); i += 2)
    {
        const std::size_t j = (i + 2 < ring.size()) ? i + 2 : 0;
        const double x0 = ring[i + 1];
        const double y0 = ring[i];
        const double x1 = ring[j + 1];
        const double y1 = ring[j];
        const double cross = x0 * y1 - x1 * y0;
        twiceArea += cross;
        cx += (x0 + x1) * cross;
        cy += (y0 + y1) * cross;
    }

    if (std::abs(twiceArea) < 1e-6)
    {
        double sumLat = 0.0;
        double sumLon = 0.0;
        std::size_t count = 0;
        for (std::size_t i = 0; i + 1 < ring.size(); i += 2)
        {
            sumLat += ring[i];
            sumLon += ring[i + 1];
            ++count;
        }
        return { static_cast<osm::Coord>(sumLat / static_cast<double>(count)),
                 static_cast<osm::Coord>(sumLon / static_cast<double>(count)) };
    }

    const double scale = 1.0 / (3.0 * twiceArea);
    return { static_cast<osm::Coord>(cy * scale), static_cast<osm::Coord>(cx * scale) };
}

} // namespace

// A `restriction` tag value -> what it means, or nothing when this build has no
// word for it.
//
// Both senses live in one function because they are the same question asked two
// ways: "no_left_turn" bans one turn, "only_straight_on" bans every other. A
// reader that handled only the first would silently permit turns an
// only_ restriction forbids.
std::optional<bool> restrictionIsOnly(std::string_view value)
{
    if (value.starts_with("no_"))
    {
        return false;
    }
    if (value.starts_with("only_"))
    {
        return true;
    }
    return std::nullopt;
}

osm::Result<ExtractStats> extract(const ExtractOptions& options, const SegmentSink& sink,
                                  const RestrictionSink& restrictionSink, const DrawSink& drawSink)
{
    ExtractStats stats;

    Mapped file(options.input);
    if (!file.valid())
    {
        return osm::io_failed("cannot read " + options.input.string());
    }

    // ---- Header ----------------------------------------------------------
    std::vector<std::uint8_t> buffer;
    {
        osm::BlobIterator it(file.bytes());
        auto blob = it.next();
        if (!blob)
        {
            return std::unexpected(blob.error());
        }
        if (blob->kind != osm::BlobKind::Header)
        {
            return osm::malformed("first blob is not an OSMHeader", blob->offset);
        }
        if (auto ok = osm::inflateBlob(*blob, buffer); !ok)
        {
            return std::unexpected(ok.error());
        }
        auto header = osm::decodeHeaderBlock(buffer);
        if (!header)
        {
            return std::unexpected(header.error());
        }
        if (auto ok = osm::checkRequiredFeatures(*header); !ok)
        {
            return std::unexpected(ok.error());
        }
        SPDLOG_INFO("[extract] {} written by '{}'", options.input.filename().string(),
                    header->writingProgram);
    }

    osm::NodeStore store;
    osm::OrderCheck order;
    TagScratch scratch;

    // Ways a relation needs the geometry of, and the subset a multipolygon has
    // claimed as part of its own area. Filled in pass 1, read in pass 3.
    std::unordered_set<std::int64_t> neededWays;
    std::unordered_set<std::int64_t> claimedWays;
    // Ways some administrative-boundary relation names. Separate from
    // claimedWays because the suppression is narrower: a boundary way that is
    // ALSO a river must still draw its river, it just must not draw its own
    // border line as well as the relation's.
    std::unordered_set<std::int64_t> boundaryMembers;

    // The geometry of those ways, held from the moment pass 3 reads them until
    // the relations that need them are read. Only the END nodes are kept
    // alongside: stitching only ever asks whether two arcs meet.
    struct MemberWay
    {
        std::int64_t firstNode { 0 };
        std::int64_t lastNode { 0 };
        std::vector<osm::Coord> geometry;
    };
    std::unordered_map<std::int64_t, MemberWay> memberWays;

    // ADMINISTRATIVE BOUNDARIES, keyed by way, holding the LOWEST admin_level
    // any relation gave that way.
    //
    // Keyed by way and not emitted inline because one way is almost always a
    // member of two relations -- the border between two counties belongs to
    // both -- and drawing it twice puts two lines on the same pixels, which at
    // any transparency reads as a heavier border on shared edges only. Lowest
    // level wins: where a state line and a county line run together, it is a
    // state line.
    std::unordered_map<std::int64_t, std::uint8_t> boundaryWays;

    // ---- Pass 1: what the ways reference ---------------------------------
    {
        osm::BlobIterator it(file.bytes());
        while (!it.done())
        {
            auto blob = it.next();
            if (!blob)
            {
                return std::unexpected(blob.error());
            }
            if (blob->kind != osm::BlobKind::Data)
            {
                continue;
            }
            if (auto ok = osm::inflateBlob(*blob, buffer); !ok)
            {
                return std::unexpected(ok.error());
            }

            // Node blocks are most of a sorted file and pass 1 has no use for
            // them; peeking steps over one for the cost of parsing its group
            // headers.
            auto contents = osm::peekDataBlock(buffer, blob->offset);
            if (!contents)
            {
                return std::unexpected(contents.error());
            }
            if (!contents->hasWays && !contents->hasRelations)
            {
                continue;
            }

            auto block = osm::decodeDataBlock(buffer, blob->offset);
            if (!block)
            {
                return std::unexpected(block.error());
            }

            for (const osm::Way& way : block->ways())
            {
                if (auto ok = order.way(way.id, blob->offset); !ok)
                {
                    return std::unexpected(ok.error());
                }
                for (const std::int64_t ref : block->refs(way))
                {
                    store.markReferenced(ref);
                }
            }
            for (const osm::Relation& relation : block->relations())
            {
                if (auto ok = order.relation(relation.id, blob->offset); !ok)
                {
                    return std::unexpected(ok.error());
                }
                for (const osm::Member& member : block->members(relation))
                {
                    if (member.type == osm::MemberType::Node)
                    {
                        store.markReferenced(member.ref);
                    }
                }

                // WHICH WAYS PASS 3 WILL HAVE TO HOLD ON TO.
                //
                // A relation's members are ways, and a way's geometry is only
                // in hand while pass 3 is looking at it -- relations come after
                // ways in the file, so by the time the relation is read the way
                // is long gone. Keeping every way would cost gigabytes, so this
                // pass works out the few that are actually needed and pass 3
                // keeps only those.
                const map_rules::TagView relationTags = scratch.view(*block, relation);
                const auto type = relationTags.get("type");
                if (!type.has_value())
                {
                    continue;
                }

                // A RELATION WITH RINGS. `type=multipolygon` is the usual
                // spelling, but a national park or a protected area is almost
                // always `type=boundary` with the same outer/inner members --
                // structurally identical, and worth assembling for exactly the
                // same reason.
                //
                // Administrative borders are the exception and are excluded
                // here: they are drawn as LINES, not as a filled shape, and
                // assembling them into an area would paint every county solid.
                const bool administrative = relationTags.is("boundary", "administrative");
                const bool multipolygon =
                    *type == "multipolygon" || (*type == "boundary" && !administrative);
                const bool boundary = *type == "boundary" && administrative;
                if (!multipolygon && !boundary)
                {
                    continue;
                }
                // A multipolygon whose own tags say nothing is the old style,
                // where the tags live on the outer way -- and that way emits
                // itself, so there is nothing here to assemble.
                //
                // "Says nothing" means DRAWN OR LABELLED, not drawn alone. A
                // nature reserve is a multipolygon carrying
                // `leisure=nature_reserve` and no fill of its own; testing only
                // for a render class discards its members here and the park
                // layer ends up with the handful mapped as single closed ways.
                const bool drawnRelation =
                    multipolygon && map_rules::classify(relationTags, { true }).drawn();
                const bool labelledRelation = multipolygon && map_rules::hasLabelTags(relationTags);
                if (multipolygon && !drawnRelation && !labelledRelation)
                {
                    continue;
                }

                for (const osm::Member& member : block->members(relation))
                {
                    if (member.type == osm::MemberType::Way)
                    {
                        neededWays.insert(member.ref);
                        if (boundary)
                        {
                            boundaryMembers.insert(member.ref);
                        }
                        // Only when the relation actually draws. A label-only
                        // relation paints nothing, so claiming its members would
                        // erase the fill they carry themselves.
                        if (drawnRelation)
                        {
                            // The relation draws this way as part of its own
                            // area, so the way must not also draw itself --
                            // that is the same lake twice, one on top of the
                            // other.
                            claimedWays.insert(member.ref);
                        }
                    }
                }
            }
        }
    }

    if (auto ok = store.finalise(); !ok)
    {
        return std::unexpected(ok.error());
    }
    stats.referencedNodes = store.stats().referenced;
    SPDLOG_INFO("[extract] pass 1: {} node ids referenced", stats.referencedNodes);

    // ---- Pass 2: which of them are junctions ------------------------------
    //
    // A node used by two or more ROUTABLE ways is where a way gets split. The
    // counter is indexed by the store's rank, which is why this cannot merge
    // with pass 1: one byte per referenced node rather than one per possible
    // id, which is 86 MB instead of 2 GB on a SoCal extract.
    std::vector<std::uint8_t> uses;
    {
        std::unordered_map<std::int64_t, std::uint32_t> rankOf;
        // The store does not expose rank directly, and it should not -- so the
        // counter is keyed by a compact index the extract assigns as it goes.
        // Ways are read once here, so each referenced node is numbered on first
        // sight and the map never grows past the referenced count.
        rankOf.reserve(static_cast<std::size_t>(stats.referencedNodes));

        osm::BlobIterator it(file.bytes());
        while (!it.done())
        {
            auto blob = it.next();
            if (!blob)
            {
                return std::unexpected(blob.error());
            }
            if (blob->kind != osm::BlobKind::Data)
            {
                continue;
            }
            if (auto ok = osm::inflateBlob(*blob, buffer); !ok)
            {
                return std::unexpected(ok.error());
            }
            auto contents = osm::peekDataBlock(buffer, blob->offset);
            if (!contents)
            {
                return std::unexpected(contents.error());
            }
            if (!contents->hasWays)
            {
                continue;
            }

            auto block = osm::decodeDataBlock(buffer, blob->offset);
            if (!block)
            {
                return std::unexpected(block.error());
            }

            for (const osm::Way& way : block->ways())
            {
                const map_rules::TagView tags = scratch.view(*block, way);
                const auto refs = block->refs(way);
                const bool closed = refs.size() > 2 && refs.front() == refs.back();
                const auto classification = map_rules::classify(tags, { closed });
                if (!classification.routable())
                {
                    continue;
                }

                for (std::size_t i = 0; i < refs.size(); ++i)
                {
                    auto [entry, inserted] = rankOf.try_emplace(
                        refs[i], static_cast<std::uint32_t>(rankOf.size()));
                    if (inserted)
                    {
                        uses.push_back(0);
                    }
                    std::uint8_t& count = uses[entry->second];
                    // The endpoints of every way are junctions whether or not
                    // anything else touches them, or a cul-de-sac would have no
                    // node to attach to.
                    const bool endpoint = (i == 0 || i + 1 == refs.size());
                    if (count < 2)
                    {
                        count = static_cast<std::uint8_t>(endpoint ? 2 : count + 1);
                    }
                }
            }
        }

        for (const std::uint8_t count : uses)
        {
            if (count >= 2)
            {
                ++stats.junctions;
            }
        }
        SPDLOG_INFO("[extract] pass 2: {} junctions among {} routable-way nodes", stats.junctions,
                    uses.size());

        // Pass 3 needs the same numbering, so it is kept.
        // (Held by value in the lambda below.)

        // EVERY LABEL LAYER, from one point.
        //
        // Shared between the node loop and the way loop because OSM tags the same
        // pharmacy either way -- as a node inside the shop, or as the building
        // outline -- and a map wants one label from it regardless. The way loop
        // reduces its shape to a point first (see labelPoint above) and then asks
        // exactly the same questions.
        //
        // A feature may land in SEVERAL of these. An airport terminal is a poi and
        // its aerodrome is an aerodrome_label; a named building with a house number
        // is both a poi and a housenumber. That is what the schema expects, and it
        // is why each check stands alone rather than sharing an else.
        const auto emitLabels = [&](const map_rules::TagView& tags, std::int64_t id, osm::Coord lat,
                                    osm::Coord lon) {
            // THE CHEAP TEST FIRST. Below this line are five classifiers and a
            // string construction, run on every tagged node and every way that
            // reached this far -- millions of buildings whose only tags are
            // `building=yes` and a height. hasLabelTags() is one pass over the
            // tags that answers "could any of them possibly say yes", and it
            // pays for itself many times over.
            if (!drawSink || !map_rules::hasLabelTags(tags))
            {
                return;
            }

            const std::string name { tags.get("name").value_or(std::string_view {}) };

            const auto emit = [&](const map_rules::LabelFeature& label, bool needsName) {
                if (!label.drawn() || (needsName && name.empty()))
                {
                    return;
                }
                DrawInput out;
                out.osmWayId = id;
                out.isPoint = true;
                out.layer = label.layer;
                out.classification.className = label.className;
                out.classification.minZoom = label.minZoom;
                out.classification.labelRank = label.rank;
                out.name = name;
                out.geometry = { lat, lon };
                if (!label.subclass.empty())
                {
                    out.attributes.emplace_back("subclass", std::string(label.subclass));
                }
                drawSink(std::move(out));
                ++stats.labels[label.layer];
            };

            // A summit without a name is a contour, not a label.
            emit(map_rules::classifyMountainPeak(tags), true);
            emit(map_rules::classifyAerodrome(tags), true);
            // A POI without a name is still a POI: a style draws the icon, and an
            // unnamed pharmacy is a pharmacy.
            emit(map_rules::classifyPoi(tags), false);

            // A HOUSE NUMBER, which is not a classification at all -- the tag IS
            // the label. By count this is the largest label layer in a suburban
            // extract, which is why it is worth its own layer rather than an
            // attribute on the building: a style draws the numbers at one zoom and
            // the buildings at several.
            if (const auto number = tags.get("addr:housenumber"); number.has_value())
            {
                DrawInput out;
                out.osmWayId = id;
                out.isPoint = true;
                out.layer = "housenumber";
                out.classification.minZoom = 14;
                out.geometry = { lat, lon };
                out.attributes.emplace_back("housenumber", std::string(*number));
                if (const auto street = tags.get("addr:street"); street.has_value())
                {
                    out.attributes.emplace_back("street", std::string(*street));
                }
                drawSink(std::move(out));
                ++stats.labels["housenumber"];
            }
        };

        // THE LAYERS A WAY FEEDS BEYOND THE ONE IT IS DRAWN IN.
        //
        // A road is drawn in `transportation` and labelled in
        // `transportation_name`; a lake is drawn in `water` and labelled in
        // `water_name`. The separation is the schema's, not ours, and it exists
        // because a label and its shape appear at different zooms and are placed
        // by different logic -- a style wants the outline of every pond at z14
        // and the name of only the large ones.
        const auto emitWayLabels = [&](const map_rules::TagView& tags, std::int64_t id,
                                       const map_rules::RoadClassification& classification,
                                       const std::vector<osm::Coord>& geom, bool closed,
                                       const std::string& name, const std::string& ref) {
            if (geom.size() < 2)
            {
                return;
            }
            const auto [lat, lon] = labelPoint(geom, closed);

            // Points of interest, house numbers and summits, from the shape's
            // label point. Identical to what a node would produce -- see
            // emitLabels.
            emitLabels(tags, id, lat, lon);

            // ROAD NAMES, as a LINE and not a point: a label renderer follows
            // the road with the text, so it needs the geometry, and the shield
            // for a numbered route hangs off the same feature. Only named or
            // numbered roads qualify -- an unnamed service road has nothing to
            // say.
            if (map_rules::isRoad(classification.renderClass) &&
                (!name.empty() || !ref.empty()))
            {
                DrawInput out;
                out.osmWayId = id;
                out.layer = "transportation_name";
                out.classification = classification;
                out.name = name;
                out.ref = ref;
                out.geometry = geom;
                drawSink(std::move(out));
                ++stats.labels["transportation_name"];
            }

            // WATER NAMES, in two shapes for two reasons.
            //
            // A lake gives a POINT, because its name sits inside it. A river
            // gives the LINE, because its name is drawn ALONG it and a renderer
            // needs the path to bend the text around -- a point would put "Santa
            // Ana River" in one spot on a watercourse forty miles long.
            //
            // The river case does NOT require a name. That looks wrong and is
            // not: the layer carries the geometry a label would follow, and the
            // name may come from a route relation or from a parent way that was
            // split, so a nameless segment of a named river still needs its
            // path in the layer for the label to run along.
            const bool waterArea = classification.renderClass == map_rules::RenderClass::Water;
            const bool waterLine = classification.renderClass == map_rules::RenderClass::Waterway;
            if ((waterArea && !name.empty()) || waterLine)
            {
                DrawInput out;
                out.osmWayId = id;
                out.layer = "water_name";
                out.classification.className = classification.className;
                // z14 only, both here and in the archive tilemaker produced.
                // A water label is detail: at z12 the lake is four pixels wide.
                out.classification.minZoom = 14;
                out.name = name;
                out.isPoint = !waterLine;
                out.geometry = waterLine ? geom : std::vector<osm::Coord> { lat, lon };
                drawSink(std::move(out));
                ++stats.labels["water_name"];
            }

            // PARKS AND PROTECTED LAND, as areas. A designation rather than a
            // ground cover: a national park is forest, rock and water at once,
            // so it is its own layer rather than a colour.
            if (closed)
            {
                if (const auto park = map_rules::classifyPark(tags); park.drawn())
                {
                    DrawInput out;
                    out.osmWayId = id;
                    out.layer = "park";
                    out.classification.renderClass = map_rules::RenderClass::Landcover;
                    out.classification.className = park.className;
                    out.classification.minZoom = park.minZoom;
                    out.classification.labelRank = park.rank;
                    out.classification.isArea = true;
                    out.closed = true;
                    out.name = name;
                    out.geometry = geom;
                    if (!park.subclass.empty())
                    {
                        out.attributes.emplace_back("subclass", std::string(park.subclass));
                    }
                    drawSink(std::move(out));
                    ++stats.labels["park"];
                }
            }

            // AIRPORT GROUND. Runways and taxiways are lines, aprons are areas,
            // and the same tag key gives both -- so the geometry decides, not
            // the classifier.
            if (const auto aeroway = map_rules::classifyAeroway(tags); aeroway.drawn())
            {
                DrawInput out;
                out.osmWayId = id;
                out.layer = "aeroway";
                out.classification.renderClass =
                    closed ? map_rules::RenderClass::Landuse : map_rules::RenderClass::Service;
                out.classification.className = aeroway.className;
                out.classification.minZoom = aeroway.minZoom;
                out.classification.isArea = closed;
                out.closed = closed;
                out.name = name;
                out.ref = ref;
                out.geometry = geom;
                if (!aeroway.subclass.empty())
                {
                    out.attributes.emplace_back("subclass", std::string(aeroway.subclass));
                }
                drawSink(std::move(out));
                ++stats.labels["aeroway"];
            }
        };

        // ---- Pass 3: coordinates, then segments --------------------------
        Bounds bounds;
        std::vector<osm::Coord> geometry;
        // A second scratch buffer, for the whole way rather than one segment of
        // it. Separate from `geometry` because the segment splitter below reuses
        // that one per segment, and sharing would leave the drawn way holding
        // whichever segment happened to be built last.
        //
        // Its capacity survives only until a DRAWN way moves it out, which is
        // deliberate: reusing the allocation on the ways that draw nothing is
        // most of the benefit, and paying a copy on the ways that do draw would
        // cost far more than the occasional regrowth.
        std::vector<osm::Coord> wayGeometry;

        osm::BlobIterator third(file.bytes());
        while (!third.done())
        {
            auto blob = third.next();
            if (!blob)
            {
                return std::unexpected(blob.error());
            }
            if (blob->kind != osm::BlobKind::Data)
            {
                continue;
            }
            if (auto ok = osm::inflateBlob(*blob, buffer); !ok)
            {
                return std::unexpected(ok.error());
            }

            auto block = osm::decodeDataBlock(buffer, blob->offset);
            if (!block)
            {
                return std::unexpected(block.error());
            }

            ++stats.blocks;
            stats.nodes += block->nodes().size();
            stats.ways += block->ways().size();
            stats.relations += block->relations().size();

            for (const osm::Relation& relation : block->relations())
            {
                const map_rules::TagView tags = scratch.view(*block, relation);

                const auto type = tags.get("type");
                if (!type.has_value())
                {
                    continue;
                }

                // ---- multipolygon areas ------------------------------------
                //
                // The relation carries the tags and the members carry the
                // shape, which is why this cannot be done a way at a time: a
                // lake is a `type=multipolygon` whose members are unordered,
                // arbitrarily-directed arcs of shoreline, plus more arcs for
                // each island. Nothing in the data says where a ring starts.
                const bool administrative = tags.is("boundary", "administrative");
                if (drawSink &&
                    (*type == "multipolygon" || (*type == "boundary" && !administrative)))
                {
                    ++stats.multipolygonsSeen;
                    const auto classification = map_rules::classify(tags, { true });
                    if (!classification.drawn() && !map_rules::hasLabelTags(tags))
                    {
                        continue;
                    }

                    std::vector<RingArc> arcs;
                    for (const osm::Member& member : block->members(relation))
                    {
                        if (member.type != osm::MemberType::Way)
                        {
                            continue;
                        }
                        auto found = memberWays.find(member.ref);
                        if (found == memberWays.end())
                        {
                            // A member outside the extract. The ring it belongs
                            // to will not close and will be dropped, which is
                            // the honest outcome at a cut edge.
                            continue;
                        }
                        RingArc arc;
                        arc.firstNode = found->second.firstNode;
                        arc.lastNode = found->second.lastNode;
                        arc.geometry = found->second.geometry;
                        const std::string_view role = block->string(member.roleIndex);
                        // An empty role means outer, which is what the old
                        // style used and what a great deal of data still says.
                        arc.inner = role == "inner";
                        arcs.push_back(std::move(arc));
                    }
                    if (arcs.empty())
                    {
                        continue;
                    }

                    auto rings = assembleRings(arcs);
                    if (rings.outer.empty())
                    {
                        ++stats.multipolygonsUnclosed;
                        continue;
                    }
                    ++stats.multipolygonsAssembled;
                    stats.multipolygonRings += rings.outer.size() + rings.inner.size();
                    if (rings.abandonedArcs != 0)
                    {
                        ++stats.multipolygonsUnclosed;
                    }

                    const std::string relationName { tags.get("name").value_or(
                        std::string_view {}) };
                    const std::string relationRef { tags.get("ref").value_or(
                        std::string_view {}) };

                    // The labels come FIRST, off the outer ring, because the
                    // drawn form below moves those rings out and there would be
                    // nothing left to take a label point from.
                    emitWayLabels(tags, relation.id, classification, rings.outer.front(), true,
                                  relationName, relationRef);

                    if (!classification.drawn())
                    {
                        // Assembled for its label alone -- a nature reserve, a
                        // national park boundary. It has a shape but no fill.
                        continue;
                    }

                    DrawInput drawn;
                    drawn.osmWayId = relation.id;
                    drawn.classification = classification;
                    drawn.closed = true;
                    drawn.name = relationName;
                    drawn.ref = relationRef;
                    drawn.geometry = std::move(rings.outer.front());
                    for (std::size_t i = 1; i < rings.outer.size(); ++i)
                    {
                        drawn.outerRings.push_back(std::move(rings.outer[i]));
                    }
                    drawn.innerRings = std::move(rings.inner);
                    drawSink(std::move(drawn));

                    ++stats.drawnWays;
                    ++stats.renderClasses[map_rules::to_string(classification.renderClass)];
                    continue;
                }

                // ---- administrative boundaries -----------------------------
                //
                // A border is a RELATION, always: the ways carry no admin_level
                // of their own, only the relation does, and a way is shared by
                // the two areas it separates. So the level has to come from
                // here even though the geometry does not.
                if (drawSink && *type == "boundary" && administrative)
                {
                    ++stats.boundaryRelationsSeen;
                    std::uint8_t level = 8;
                    if (const auto text = tags.get("admin_level"); text.has_value())
                    {
                        unsigned parsed = 0;
                        const auto [ptr, ec] = std::from_chars(
                            text->data(), text->data() + text->size(), parsed);
                        (void)ptr;
                        if (ec != std::errc {} || parsed == 0 || parsed > 12)
                        {
                            // Unparseable or out of range. Skipped rather than
                            // defaulted: an unknown level drawn at the default
                            // puts an arbitrary line at city weight across the
                            // map, and a missing border is easier to notice.
                            ++stats.boundaryRelationsUnrecognised;
                            continue;
                        }
                        level = static_cast<std::uint8_t>(parsed);
                    }

                    for (const osm::Member& member : block->members(relation))
                    {
                        if (member.type != osm::MemberType::Way)
                        {
                            continue;
                        }
                        auto [entry, inserted] = boundaryWays.emplace(member.ref, level);
                        if (!inserted && level < entry->second)
                        {
                            entry->second = level;
                        }
                    }
                    continue;
                }

                if (!type->starts_with("restriction"))
                {
                    continue;
                }
                ++stats.restrictionsSeen;

                // The value may live under `restriction` or a mode-specific
                // key. Only the general one is honoured: a restriction that
                // applies to lorries and not cars would otherwise be applied to
                // everything.
                const auto value = tags.get("restriction");
                if (!value.has_value())
                {
                    ++stats.restrictionsUnrecognised;
                    continue;
                }
                const auto only = restrictionIsOnly(*value);
                if (!only.has_value())
                {
                    ++stats.restrictionsUnrecognised;
                    continue;
                }

                road_graph::Builder::RestrictionInput out;
                out.only = *only;
                bool viaWay = false;
                bool complete = true;

                for (const osm::Member& member : block->members(relation))
                {
                    const std::string_view role = block->string(member.roleIndex);
                    if (role == "from" && member.type == osm::MemberType::Way)
                    {
                        out.fromWayId = member.ref;
                    }
                    else if (role == "to" && member.type == osm::MemberType::Way)
                    {
                        out.toWayId = member.ref;
                    }
                    else if (role == "via")
                    {
                        if (member.type == osm::MemberType::Node)
                        {
                            out.viaNodeId = member.ref;
                        }
                        else
                        {
                            // A via-WAY restriction spans a path rather than a
                            // junction. Counted, not guessed: picking a nearby
                            // node would ban a turn somewhere else on the same
                            // road, which is a route that silently avoids a
                            // legal manoeuvre.
                            viaWay = true;
                        }
                    }
                }

                if (viaWay)
                {
                    ++stats.restrictionsViaWay;
                    continue;
                }
                complete = out.fromWayId != 0 && out.toWayId != 0 && out.viaNodeId != 0;
                if (!complete)
                {
                    ++stats.restrictionsUnrecognised;
                    continue;
                }

                ++stats.restrictionsViaNode;
                if (restrictionSink)
                {
                    restrictionSink(out);
                }
            }

            if (options.progressEvery != 0 && stats.blocks % options.progressEvery == 0)
            {
                SPDLOG_INFO("[extract] pass 3: {} blocks, {} segments", stats.blocks,
                            stats.segments);
            }

            for (const osm::Node& node : block->nodes())
            {
                store.set(node.id, node.lat, node.lon);
                bounds.grow(node.lat, node.lon);

                // LABELS. Places are the only thing here a node has a monopoly
                // on -- no way is tagged place=city -- but a node carries points
                // of interest, summits and house numbers too, and this is the
                // one point in the extraction where node tags are read at all.
                //
                // Cheap by construction: the overwhelming majority of nodes are
                // untagged geometry vertices, and tagCount is already decoded.
                if (!drawSink || node.tagCount == 0)
                {
                    continue;
                }

                const map_rules::TagView nodeTags = scratch.view(*block, node);
                emitLabels(nodeTags, node.id, node.lat, node.lon);

                const auto place = map_rules::classifyPlace(nodeTags);
                if (!place.drawn())
                {
                    continue;
                }

                DrawInput label;
                label.osmWayId = node.id;
                label.isPoint = true;
                label.place = place;
                label.classification.renderClass = map_rules::RenderClass::Place;
                label.classification.minZoom = place.minZoom;
                label.classification.labelRank = place.labelRank;
                label.geometry = { node.lat, node.lon };
                if (auto name = nodeTags.get("name"))
                {
                    label.name = std::string(*name);
                }
                drawSink(std::move(label));

                ++stats.places;
                ++stats.renderClasses[map_rules::to_string(map_rules::RenderClass::Place)];
            }

            for (const osm::Way& way : block->ways())
            {
                const map_rules::TagView tags = scratch.view(*block, way);
                const auto refs = block->refs(way);
                if (refs.size() < 2)
                {
                    continue;
                }
                const bool closed = refs.size() > 2 && refs.front() == refs.back();
                const auto classification = map_rules::classify(tags, { closed });

                if (classification.drawn())
                {
                    ++stats.drawnWays;
                    ++stats.renderClasses[map_rules::to_string(classification.renderClass)];
                }
                if (classification.routable())
                {
                    ++stats.routableWays;
                    ++stats.routeClasses[map_rules::to_string(classification.routeClass)];
                }

                const bool needed = drawSink && neededWays.contains(way.id);

                // A way earns its coordinates by being drawn, by being routable,
                // by belonging to a relation -- or by carrying a LABEL.
                //
                // That last clause is not obvious and its absence is silent. A
                // runway is `aeroway=runway` and nothing else: no render class,
                // no route class, no relation. Without this it is discarded
                // here, several hundred lines before anything asks whether it
                // is a label, and the airport comes out with no tarmac and no
                // error anywhere. The same is true of every POI mapped as an
                // outline rather than a point.
                const bool labelled = drawSink && map_rules::hasLabelTags(tags);
                if (!classification.drawn() && !classification.routable() && !needed && !labelled)
                {
                    continue;
                }

                // Resolve every vertex FIRST. A single unresolved one drops the
                // WHOLE WAY: truncating it instead would leave the graph
                // disconnected at the seam, which is worse than a missing road
                // because it is invisible -- the map still draws a road and the
                // router simply never uses it.
                bool complete = true;
                osm::Coord anyLat = 0;
                osm::Coord anyLon = 0;
                for (const std::int64_t ref : refs)
                {
                    auto coord = store.get(ref);
                    if (!coord)
                    {
                        complete = false;
                        continue;
                    }
                    anyLat = coord->first;
                    anyLon = coord->second;
                }

                if (!complete)
                {
                    if (bounds.valid() && (anyLat != 0 || anyLon != 0) &&
                        !bounds.nearEdge(anyLat, anyLon))
                    {
                        ++stats.droppedInInterior;
                    }
                    else
                    {
                        ++stats.droppedAtBoundary;
                    }
                    continue;
                }

                const std::string name =
                    std::string(tags.get("name").value_or(std::string_view {}));
                const std::string ref =
                    std::string(tags.get("ref").value_or(std::string_view {}));

                // The drawn form: the WHOLE way, unsplit. One classify() call,
                // two consumers -- which is exactly the thing owning the
                // extractor buys, and the reason a route can never use a road
                // the tiler did not draw.
                if (needed)
                {
                    MemberWay member;
                    member.firstNode = refs.front();
                    member.lastNode = refs.back();
                    member.geometry.reserve(refs.size() * 2);
                    for (const std::int64_t r : refs)
                    {
                        auto coord = store.get(r);
                        member.geometry.push_back(coord->first);
                        member.geometry.push_back(coord->second);
                    }
                    memberWays.emplace(way.id, std::move(member));
                }

                // A way a multipolygon has claimed does not draw itself: the
                // relation draws it, as part of an area that may have holes.
                // Emitting both puts the lake on top of its own island.
                const bool claimed = claimedWays.contains(way.id);
                // A boundary way whose level comes from a relation is drawn by
                // that relation, at that relation's level. Letting it draw
                // itself as well puts a second line on the same pixels -- and
                // at a default level, since the way itself carries none.
                const bool relationDrawsTheBorder =
                    classification.renderClass == map_rules::RenderClass::Boundary &&
                    boundaryMembers.contains(way.id);

                if (drawSink)
                {
                    wayGeometry.clear();
                    wayGeometry.reserve(refs.size() * 2);
                    for (const std::int64_t r : refs)
                    {
                        auto coord = store.get(r);
                        wayGeometry.push_back(coord->first);
                        wayGeometry.push_back(coord->second);
                    }

                    // LABELS FIRST, then the drawn feature -- and the order is
                    // the whole reason this reads oddly.
                    //
                    // Both want the same coordinates. Emitting the drawn way
                    // first would mean either copying the buffer or handing it
                    // away, and a copy here is a copy per drawn way: eleven
                    // million of them on a regional extract, for nothing.
                    // Labelling first lets the drawn way MOVE the buffer out.
                    emitWayLabels(tags, way.id, classification, wayGeometry, closed, name, ref);

                    if (classification.drawn() && !claimed && !relationDrawsTheBorder)
                    {
                        DrawInput drawn;
                        drawn.osmWayId = way.id;
                        drawn.classification = classification;
                        drawn.name = name;
                        drawn.ref = ref;
                        drawn.closed = closed;
                        drawn.geometry = std::move(wayGeometry);
                        drawSink(std::move(drawn));
                    }
                }

                if (!classification.routable())
                {
                    continue;
                }

                // Split at junctions. The ordinal counts along the way, so
                // SegmentId is stable and segmentsOfWay() comes back ordered.
                std::uint32_t ordinal = 0;
                std::size_t start = 0;
                for (std::size_t i = 1; i < refs.size(); ++i)
                {
                    const auto found = rankOf.find(refs[i]);
                    const bool junction =
                        (i + 1 == refs.size()) ||
                        (found != rankOf.end() && uses[found->second] >= 2);
                    if (!junction)
                    {
                        continue;
                    }

                    geometry.clear();
                    for (std::size_t g = start; g <= i; ++g)
                    {
                        auto coord = store.get(refs[g]);
                        geometry.push_back(coord->first);
                        geometry.push_back(coord->second);
                    }

                    if (geometry.size() >= 4)
                    {
                        road_graph::Builder::SegmentInput input;
                        input.id = road_graph::makeSegmentId(way.id, ordinal);
                        input.osmWayId = way.id;
                        input.fromNodeId = refs[start];
                        input.toNodeId = refs[i];
                        input.geometry = geometry;
                        input.classification = classification;
                        input.name = name;
                        input.ref = ref;
                        sink(std::move(input));
                        ++stats.segments;
                        ++ordinal;
                    }

                    start = i;
                }
            }
        }

        if (bounds.valid())
        {
            stats.west = bounds.west;
            stats.south = bounds.south;
            stats.east = bounds.east;
            stats.north = bounds.north;
        }
    }

    // ---- The boundaries, once each ---------------------------------------
    //
    // Deferred to here rather than emitted in the relation loop because a way
    // is not finished being described until every relation has been read: the
    // second relation naming it may be the one with the lower admin_level, and
    // it may live in a later block. Emitting on first sight would draw the
    // county line and never learn it was also the state line.
    for (const auto& [wayId, level] : boundaryWays)
    {
        auto found = memberWays.find(wayId);
        if (found == memberWays.end() || found->second.geometry.size() < 4)
        {
            // Outside the extract, or too short to be a line. Normal at a cut
            // edge -- a border is exactly the thing an extract is cut along.
            ++stats.boundaryWaysMissing;
            continue;
        }

        DrawInput drawn;
        drawn.osmWayId = wayId;
        drawn.classification.renderClass = map_rules::RenderClass::Boundary;
        drawn.classification.className = "administrative";
        // A national border is visible on a world tile; a city limit is not
        // worth drawing until the city fills the screen. Levels are OSM's
        // 2/4/6/8 for country/state/county/city, with the odd levels between
        // them taking the coarser neighbour's zoom.
        drawn.classification.minZoom = (level <= 2) ? 0 : (level <= 4) ? 4 : (level <= 6) ? 8 : 10;
        drawn.adminLevel = level;
        drawn.geometry = found->second.geometry;
        drawSink(std::move(drawn));
        ++stats.boundaryWaysDrawn;
        ++stats.renderClasses[map_rules::to_string(map_rules::RenderClass::Boundary)];
    }

    stats.resolvedNodes = store.stats().resolved;
    stats.nodeStoreBytes = store.stats().bytes;
    return stats;
}

} // namespace map_build
