// SPDX-License-Identifier: GPL-3.0-or-later
#include "map_rules/labels.h"

#include <array>

namespace map_rules
{
namespace
{

// The keys a POI can be spelled under, with the coarse bucket each maps to and
// the zoom it first appears at.
//
// ORDER IS THE PRIORITY. A village shop inside a hospital compound carries both
// `shop` and `amenity`, and the first match wins -- so the more specific key is
// listed first. Getting this backwards labels the shop as the hospital.
struct PoiKey
{
    std::string_view key;
    const char* className;
    std::uint8_t minZoom;
    std::uint8_t rank;
};

constexpr std::array<PoiKey, 9> kPoiKeys { {
    { "aeroway", "aeroway", 13, 10 },
    { "railway", "railway", 13, 12 },
    { "shop", "shop", 14, 30 },
    { "tourism", "tourism", 13, 20 },
    { "historic", "historic", 14, 25 },
    { "office", "office", 14, 35 },
    { "leisure", "leisure", 14, 28 },
    { "amenity", "amenity", 14, 22 },
    { "landuse", "landuse", 14, 40 },
} };

// Values that are NOT points of interest even though they live on a POI key.
//
// Without this the poi layer fills with the fabric of the city rather than the
// things in it: every parking aisle, every driveway gate, every stretch of
// grass. They are drawn already, by the landcover and transportation rules --
// this layer is for things a person would go TO.
bool isBackground(std::string_view key, std::string_view value)
{
    if (key == "leisure")
    {
        return value == "park" || value == "garden" || value == "pitch" ||
               value == "nature_reserve" || value == "common" || value == "swimming_pool";
    }
    if (key == "landuse")
    {
        // Only the institutional campuses are worth a label; the rest of the key
        // is ground cover.
        return !(value == "cemetery" || value == "quarry" || value == "military");
    }
    if (key == "amenity")
    {
        return value == "parking_space" || value == "bench" || value == "waste_basket" ||
               value == "bicycle_parking" || value == "grit_bin" || value == "drinking_water";
    }
    if (key == "railway")
    {
        // The track itself is transportation. Only somewhere you board counts.
        return !(value == "station" || value == "halt" || value == "tram_stop" ||
                 value == "subway_entrance");
    }
    if (key == "aeroway")
    {
        // The aerodrome gets its own label layer, and the tarmac gets the
        // aeroway layer. What is left here is the terminal and the gate.
        return !(value == "terminal" || value == "gate" || value == "helipad");
    }
    if (key == "tourism")
    {
        return value == "yes";
    }
    return false;
}

// Every key any classifier in this file reads. Kept beside them deliberately:
// a classifier that starts reading a new key and is not listed here goes on
// answering correctly for nodes and silently never being asked about ways.
constexpr std::array<std::string_view, 12> kLabelKeys { {
    "aeroway", "railway", "shop", "tourism", "historic", "office",
    "leisure", "amenity", "landuse", "natural", "boundary", "addr:housenumber",
} };

} // namespace

bool hasLabelTags(const TagView& tags)
{
    for (const auto& [key, value] : tags.pairs)
    {
        (void)value;
        for (const std::string_view candidate : kLabelKeys)
        {
            if (key == candidate)
            {
                return true;
            }
        }
    }
    return false;
}

LabelFeature classifyPoi(const TagView& tags)
{
    for (const PoiKey& entry : kPoiKeys)
    {
        const auto value = tags.get(entry.key);
        if (!value.has_value() || value->empty() || *value == "no")
        {
            continue;
        }
        if (isBackground(entry.key, *value))
        {
            continue;
        }

        LabelFeature out;
        out.layer = "poi";
        out.className = entry.className;
        out.subclass = *value;
        out.minZoom = entry.minZoom;
        out.rank = entry.rank;
        return out;
    }
    return {};
}

LabelFeature classifyAeroway(const TagView& tags)
{
    const auto value = tags.get("aeroway");
    if (!value.has_value() || value->empty())
    {
        return {};
    }
    // A terminal, a gate and a control tower are BUILDINGS, and are drawn as
    // such. Leaving them here would put the terminal's footprint in the same
    // layer as the tarmac, where a style paints it concrete.
    if (*value == "terminal" || *value == "gate" || *value == "tower")
    {
        return {};
    }

    LabelFeature out;
    out.layer = "aeroway";
    out.subclass = *value;

    // THE VALUE IS THE CLASS, passed through rather than bucketed -- the same
    // open-vocabulary choice made for landuse. The named cases below exist only
    // to set a zoom: a runway at a major airport is kilometres of pale concrete
    // and is the landmark that identifies the airport from the air, while a
    // holding position is z14 detail. Everything unlisted gets the detail zoom,
    // which is the safe direction to be wrong in.
    if (*value == "runway")
    {
        out.className = "runway";
        out.minZoom = 10;
        out.rank = 1;
        return out;
    }
    if (*value == "aerodrome")
    {
        out.className = "aerodrome";
        out.minZoom = 11;
        out.rank = 2;
        return out;
    }
    if (*value == "taxiway")
    {
        out.className = "taxiway";
        out.minZoom = 13;
        out.rank = 3;
        return out;
    }
    if (*value == "apron")
    {
        out.className = "apron";
        out.minZoom = 13;
        out.rank = 4;
        return out;
    }
    if (*value == "helipad")
    {
        out.className = "helipad";
        out.minZoom = 14;
        out.rank = 5;
        return out;
    }
    out.className = "other";
    out.minZoom = 14;
    out.rank = 6;
    return out;
}

LabelFeature classifyAerodrome(const TagView& tags)
{
    if (!tags.is("aeroway", "aerodrome"))
    {
        return {};
    }

    LabelFeature out;
    out.layer = "aerodrome_label";
    // An international airport is a navigational landmark at continental zoom; a
    // private airstrip is not. `aerodrome:type` is the tag that separates them
    // and is widely set, so it is worth reading rather than treating every
    // airfield alike.
    const auto kind = tags.get("aerodrome:type").value_or(tags.get("aerodrome").value_or(""));
    const bool international = kind == "international" || tags.has("iata");
    out.className = international ? "international" : "other";
    out.subclass = kind;
    out.minZoom = international ? 8 : 12;
    out.rank = international ? 1 : 5;
    return out;
}

LabelFeature classifyPark(const TagView& tags)
{
    LabelFeature out;
    out.layer = "park";

    // DELIBERATELY NARROW: a designation over terrain, not any green space.
    //
    // `leisure=park` is NOT here, and that is the point -- a city park is
    // already ground cover and drawing it in both layers paints it twice, once
    // as grass and once as park, which at any transparency reads as a
    // colour-banding bug along every park edge. The same argument excludes
    // `boundary=protected_area`, which in the US covers whole national forests
    // and would put a wash over half the map.
    // US NATIONAL FORESTS ARE EXCLUDED, deliberately.
    //
    // They are tagged as protected areas and they are enormous -- the Angeles,
    // San Bernardino and Cleveland forests alone cover most of the mountains
    // behind Los Angeles. Drawing them puts a green wash over half of Southern
    // California, under which the roads a driver is actually looking at become
    // hard to read. This is a cartographic decision rather than a data one, and
    // it is the same one tilemaker's OpenMapTiles profile makes.
    if (tags.is("protection_title", "National Forest"))
    {
        return {};
    }

    if (tags.is("boundary", "national_park"))
    {
        out.className = "national_park";
        out.subclass = "national_park";
        out.minZoom = 5;
        out.rank = 1;
        return out;
    }
    if (tags.is("leisure", "nature_reserve"))
    {
        out.className = "nature_reserve";
        out.subclass = "nature_reserve";
        out.minZoom = 8;
        out.rank = 2;
        return out;
    }
    return {};
}

LabelFeature classifyMountainPeak(const TagView& tags)
{
    const auto natural = tags.get("natural");
    if (!natural.has_value())
    {
        return {};
    }
    if (*natural != "peak" && *natural != "volcano" && *natural != "saddle" &&
        *natural != "ridge")
    {
        return {};
    }

    LabelFeature out;
    out.layer = "mountain_peak";
    out.className = (*natural == "volcano") ? "volcano" : "peak";
    out.subclass = *natural;
    out.minZoom = 11;
    out.rank = (*natural == "volcano") ? 1 : 3;
    return out;
}

} // namespace map_rules
