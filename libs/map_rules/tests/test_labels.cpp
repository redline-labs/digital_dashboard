// SPDX-License-Identifier: GPL-3.0-or-later
//
// The label vocabulary, and specifically the decisions that are invisible when
// wrong.
//
// Every case here was found by counting features against tilemaker's output on
// a real Southern California extract. None of them produced an error, a warning
// or a crash -- each one produced a map that was quietly missing a layer, or
// carrying one twice:
//
//   - a runway is `aeroway=runway` and NOTHING ELSE. It has no render class and
//     no route class, so an extractor that keeps a way only when it is drawn or
//     routable discards it before any label rule is asked. The airport comes out
//     with no tarmac. hasLabelTags() is the gate that prevents it, and the test
//     that matters is that the gate agrees with the classifiers.
//
//   - a nature reserve must NOT also be ground cover. It is a designation over
//     terrain that is forest, rock and water at once, and filling it green
//     states something about the ground that is not true -- as well as drawing
//     the same shape in two layers.
//
//   - requiring a NAME loses a third of the summits and four fifths of the
//     airfields. A style draws the icon and omits the text; the feature still
//     belongs in the layer.

#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#include "map_rules/classification.h"
#include "map_rules/labels.h"

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

using Pairs = std::vector<std::pair<std::string_view, std::string_view>>;

map_rules::TagView view(const Pairs& pairs)
{
    return map_rules::TagView { pairs };
}

void test_a_runway_is_a_label_even_though_nothing_draws_it()
{
    const Pairs runway { { "aeroway", "runway" } };

    const auto classified = map_rules::classify(view(runway), { false });
    check(!classified.drawn(), "a runway has no render class -- nothing fills it");
    check(!classified.routable(), "and no route class -- a car does not drive down it");

    // Which is exactly why the two below have to be true, or the way is thrown
    // away before anyone asks.
    check(map_rules::hasLabelTags(view(runway)), "but hasLabelTags() keeps it");
    const auto label = map_rules::classifyAeroway(view(runway));
    check(label.drawn(), "and classifyAeroway() claims it");
    check(std::string_view(label.layer) == "aeroway", "into the aeroway layer");
    check(std::string_view(label.className) == "runway", "as class=runway");
}

void test_the_gate_agrees_with_every_classifier()
{
    // The failure this guards is one-directional and silent: a classifier that
    // starts reading a key hasLabelTags() does not list keeps working perfectly
    // for NODES -- which are never gated -- and silently stops being asked about
    // WAYS. Half the features appear, which reads as sparse data.
    const std::vector<Pairs> cases {
        { { "aeroway", "taxiway" } },
        { { "aeroway", "aerodrome" } },
        { { "natural", "peak" } },
        { { "natural", "volcano" } },
        { { "boundary", "national_park" } },
        { { "leisure", "nature_reserve" } },
        { { "shop", "bakery" } },
        { { "amenity", "pharmacy" } },
        { { "tourism", "museum" } },
        { { "historic", "monument" } },
        { { "office", "lawyer" } },
        { { "railway", "station" } },
        { { "landuse", "cemetery" } },
        { { "addr:housenumber", "1600" } },
    };

    for (const Pairs& tags : cases)
    {
        const bool anyClassifier = map_rules::classifyPoi(view(tags)).drawn() ||
                                   map_rules::classifyAeroway(view(tags)).drawn() ||
                                   map_rules::classifyAerodrome(view(tags)).drawn() ||
                                   map_rules::classifyPark(view(tags)).drawn() ||
                                   map_rules::classifyMountainPeak(view(tags)).drawn() ||
                                   view(tags).has("addr:housenumber");
        const bool gate = map_rules::hasLabelTags(view(tags));
        check(!anyClassifier || gate, std::string("hasLabelTags() lets '") +
                                          std::string(tags.front().first) + "=" +
                                          std::string(tags.front().second) + "' through");
    }
}

void test_a_nature_reserve_is_a_park_and_not_ground_cover()
{
    const Pairs reserve { { "leisure", "nature_reserve" }, { "name", "Bolsa Chica" } };

    const auto park = map_rules::classifyPark(view(reserve));
    check(park.drawn(), "a nature reserve is a park");
    check(std::string_view(park.className) == "nature_reserve", "of class nature_reserve");

    // The other half, and the one that actually went wrong: it must not ALSO be
    // filled as landcover, or the same outline is drawn twice in two layers and
    // a style that paints both gets a colour band along every reserve edge.
    const auto drawn = map_rules::classify(view(reserve), { true });
    check(!drawn.drawn(), "and it is NOT drawn as ground cover as well");
}

void test_a_city_park_is_ground_cover_and_not_a_park_layer_feature()
{
    // The mirror image, and the reason the park layer is deliberately narrow:
    // `leisure=park` IS green ground and belongs in landcover. Putting it in
    // both layers is the same double-draw from the other direction.
    const Pairs park { { "leisure", "park" }, { "name", "Central Park" } };

    check(!map_rules::classifyPark(view(park)).drawn(),
          "a city park is not a park-layer feature");
    const auto drawn = map_rules::classify(view(park), { true });
    check(drawn.drawn(), "it is ground cover");
    check(std::string_view(drawn.className) == "grass", "of class grass");
}

void test_a_summit_needs_no_name()
{
    const auto peak = map_rules::classifyMountainPeak(view({ { "natural", "peak" } }));
    check(peak.drawn(), "an unnamed summit is still a summit");
    check(std::string_view(peak.className) == "peak", "of class peak");

    const auto volcano = map_rules::classifyMountainPeak(view({ { "natural", "volcano" } }));
    check(std::string_view(volcano.className) == "volcano", "a volcano says so");

    check(!map_rules::classifyMountainPeak(view({ { "natural", "water" } })).drawn(),
          "and a lake is not a summit");
}

void test_a_terminal_is_a_building_not_tarmac()
{
    // aeroway=terminal is the passenger building. Left in the aeroway layer a
    // style paints its footprint as concrete, in the middle of the apron.
    check(!map_rules::classifyAeroway(view({ { "aeroway", "terminal" } })).drawn(),
          "a terminal is not aeroway tarmac");
    check(!map_rules::classifyAeroway(view({ { "aeroway", "gate" } })).drawn(),
          "nor is a gate");

    // But an unlisted value still gets through, because the vocabulary is open:
    // a holding position and a jet bridge are both real airport surface.
    const auto holding = map_rules::classifyAeroway(view({ { "aeroway", "holding_position" } }));
    check(holding.drawn(), "an unlisted aeroway value is still aeroway");
    check(holding.subclass == "holding_position", "and carries its own value as subclass");
}

void test_swimming_pools_are_water()
{
    // The single rule that moved water from 23 000 features to 78 000 on a
    // Southern California extract. A pool is `leisure=swimming_pool`, and a rule
    // that reaches the leisure key before the water key files every one of them
    // as ground cover -- in a region that has hundreds of thousands of them.
    const auto pool = map_rules::classify(view({ { "leisure", "swimming_pool" } }), { true });
    check(pool.renderClass == map_rules::RenderClass::Water, "a swimming pool is water");
    check(std::string_view(pool.className) == "lake", "of class lake");

    const auto basin = map_rules::classify(view({ { "landuse", "basin" } }), { true });
    check(basin.renderClass == map_rules::RenderClass::Water, "so is a basin");

    // A waterway mapped as a shape rather than a line.
    const auto riverbank = map_rules::classify(view({ { "waterway", "riverbank" } }), { true });
    check(riverbank.renderClass == map_rules::RenderClass::Water, "and a riverbank");
    check(std::string_view(riverbank.className) == "river", "as moving water");

    // The same tag NOT closed is a line, and must stay one.
    const auto stream = map_rules::classify(view({ { "waterway", "stream" } }), { false });
    check(stream.renderClass == map_rules::RenderClass::Waterway,
          "an open waterway is still a line");
}

void test_a_class_name_is_never_borrowed_from_the_tags()
{
    // className is a `const char*` that outlives the TagView it came from -- the
    // tags are views into a PBF block the extractor drops. A branch that
    // returned a pointer into the tag text would read freed memory later, and
    // string_view is not null-terminated either, so it would read PAST it.
    //
    // Checked by building the classification from tags that are then destroyed.
    const char* className = nullptr;
    {
        const Pairs scoped { { "waterway", "canal" } };
        className = map_rules::classify(view(scoped), { false }).className;
    }
    check(std::string_view(className) == "canal",
          "a class name survives the tags it was derived from");
}

} // namespace

int main()
{
    spdlog::set_level(spdlog::level::warn);
    spdlog::set_pattern("[%^%l%$] %v");

    test_a_runway_is_a_label_even_though_nothing_draws_it();
    test_the_gate_agrees_with_every_classifier();
    test_a_nature_reserve_is_a_park_and_not_ground_cover();
    test_a_city_park_is_ground_cover_and_not_a_park_layer_feature();
    test_a_summit_needs_no_name();
    test_a_terminal_is_a_building_not_tarmac();
    test_swimming_pools_are_water();
    test_a_class_name_is_never_borrowed_from_the_tags();

    spdlog::set_level(spdlog::level::info);
    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }

    SPDLOG_INFO("all label classification checks passed");
    return 0;
}
