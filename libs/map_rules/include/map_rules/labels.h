// SPDX-License-Identifier: GPL-3.0-or-later
//
// The label vocabulary: points of interest, peaks, airports, parks, house
// numbers -- everything a map puts a WORD on rather than a shape.
//
// SEPARATE FROM classification.h ON PURPOSE, and the reason is that the two
// have opposite shapes.
//
// RenderClass is a closed enum of about twenty values, switched exhaustively in
// the tessellator, the style struct and the tiler, so that -Wswitch-enum catches
// a class nobody handled. That is the right trade for things that are DRAWN:
// there are only so many ways to paint a line.
//
// Labels are the opposite. OSM's `amenity`, `shop` and `tourism` keys carry
// several thousand values between them, the list grows every week, and a style
// that does not recognise one simply draws no icon. Forcing that into an
// exhaustive enum would mean a five-file edit every time a new kind of shop is
// tagged, to gain nothing -- so the raw value passes through as `subclass` and
// only the coarse bucket is closed.
//
// Neither of these feeds the road graph. A router does not care that a building
// is a pharmacy.
#ifndef MAP_RULES_LABELS_H
#define MAP_RULES_LABELS_H

#include <cstdint>
#include <string_view>

#include "map_rules/classification.h"

namespace map_rules
{

// One label feature: which tile layer it belongs in and what it says.
struct LabelFeature
{
    // The tile layer name, in the OpenMapTiles vocabulary. Empty means "this is
    // not a label feature", which is the answer for the overwhelming majority
    // of entities and is why every classifier here returns by value.
    const char* layer { "" };

    // The coarse bucket, from a closed list. A style colours by this.
    const char* className { "" };

    // OSM'S OWN VALUE, PASSED THROUGH -- and therefore a BORROWED view into the
    // tags that were handed in, valid only as long as they are.
    //
    // That is a real constraint in this tree, because tags are string_views into
    // a decompressed PBF block that the extractor drops as soon as it moves to
    // the next one. Copy it before the block goes; do not store this.
    std::string_view subclass;

    std::uint8_t minZoom { 255 };

    // Lower sorts first when labels collide, matching labelRank elsewhere.
    std::uint8_t rank { 255 };

    bool drawn() const { return layer[0] != '\0'; }
};

// WHETHER ANY LABEL CLASSIFIER COULD SAY YES.
//
// A cheap gate, and it exists because of a failure that is entirely silent: an
// extractor naturally skips a way that is neither drawn nor routable, and a
// runway is exactly that -- `aeroway=runway` has no render class and no route
// class, so it is discarded before anything asks whether it is a label. The
// symptom is an airport with no tarmac and no error anywhere.
//
// One pass over the tags rather than running every classifier, because this is
// asked of every way in the file and the classifiers are not.
bool hasLabelTags(const TagView& tags);

// A point of interest -- a shop, a school, a restaurant, a station. Answers for
// both nodes and areas: OSM tags the same pharmacy either way, and a map wants
// one label from it regardless, so the caller reduces an area to a label point
// and asks the same question.
LabelFeature classifyPoi(const TagView& tags);

// Airport ground infrastructure: runways, taxiways, aprons, helipads. Lines and
// areas both, which is why this does not take a Shape -- the geometry decides
// how it is drawn, not whether it is drawn.
LabelFeature classifyAeroway(const TagView& tags);

// An airport itself, as a label rather than as tarmac.
LabelFeature classifyAerodrome(const TagView& tags);

// Parks and protected land. Distinct from landcover=grass: a national park is a
// designation over terrain that may be forest, rock and water at once, so it is
// a layer of its own rather than a colour.
LabelFeature classifyPark(const TagView& tags);

// A named summit. `ele` is not parsed here -- it is carried through as an
// attribute by the caller, because its unit handling (metres, feet, and the
// occasional "1234 m") is a display concern.
LabelFeature classifyMountainPeak(const TagView& tags);

} // namespace map_rules

#endif // MAP_RULES_LABELS_H
