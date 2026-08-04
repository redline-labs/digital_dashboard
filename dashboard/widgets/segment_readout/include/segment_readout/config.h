#ifndef SEGMENT_READOUT_CONFIG_H
#define SEGMENT_READOUT_CONFIG_H

#include <cstdint>
#include <string>
#include <vector>

#include "helpers/color.h"
#include "pub_sub/schema_registry.h"
#include "reflection/reflection.h"
#include "dashboard/config_limits.h"

// Which DSEG face the readout is set in. Seven-segment can only render digits;
// fourteen-segment can render letters, which is what the CDL3's alphanumeric
// fields use.
REFLECT_ENUM(SegmentFace,
    seven,
    fourteen
)

// Where the caption sits relative to the value. The CDL3 uses both: "FUEL" sits
// to the right of the fuel figure, "TIME" sits above the lap time.
REFLECT_ENUM(SegmentCaptionPosition,
    right,
    top
)

// A segmented-LCD readout, drawn the way a real one looks: the unlit segments
// of every cell are visible as faint "ghosts" behind the lit ones.
//
// That ghosting is the single most recognisable trait of the CDL3's screen, and
// it is not something a plain text label can imitate -- the unlit segments are
// physically there on the glass whether or not they are driven. Here it is drawn
// by rendering a full-house string ("8888", "~~~~") in the ghost colour and then
// the live value over the top, in the same face at the same position, so the two
// line up cell for cell.
REFLECT_STRUCT(SegmentReadoutConfig_t,
    (std::string, zenoh_key, ""),
    (pub_sub::schema_type_t, schema_type, pub_sub::schema_type_t::VehicleSpeed),
    (std::string, value_expression, ""),

    // Shown instead of a subscribed value when there is no expression. This is
    // how the fixed alphanumeric fields ("OILPRESS") are set.
    (std::string, static_text, ""),

    // Fixed text held in the leading cells, with the value right-aligned in what
    // is left. This is what the CDL3's alphanumeric bar actually does: it is one
    // 13-character field reading "OIL PRESS   85", not a label widget sitting
    // next to a number widget. Drawing it as two fields gets the cells wrong,
    // because each field would round its own cell width independently.
    (std::string, prefix, ""),

    (SegmentFace, face, SegmentFace::seven),
    // Number of cells. The ghost string is this many full-house characters, so
    // it also fixes how wide the readout draws regardless of the current value.
    (uint16_t, digits, 4),
    (uint16_t, decimals, 0),

    (helpers::Color, lit_color, "#101820"),
    // The unlit segments. Low contrast against the backlight on purpose; too
    // strong and it reads as garbage text rather than as an idle display.
    (helpers::Color, ghost_color, "#5AB4BE"),
    (bool, show_ghosts, true),

    // Small caption drawn alongside, like the CDL3's "FUEL" and "TIME".
    (std::string, caption, ""),
    (SegmentCaptionPosition, caption_position, SegmentCaptionPosition::right),
    (helpers::Color, caption_color, "#101820")
)

REFLECT_METADATA(SegmentReadoutConfig_t,
    (zenoh_key, "Zenoh Key", "Zenoh topic key to subscribe to"),
    (schema_type, "Schema Type", "Data schema type for the subscription"),
    (value_expression, "Value Expression", "Expression to compute the displayed value"),
    (static_text, "Static Text", "Fixed text to display when there is no expression"),
    (prefix, "Prefix", "Fixed text in the leading cells, value right-aligned after it"),
    (face, "Face", "seven for digits only, fourteen for alphanumerics"),
    (digits, "Digits", "Number of cells, which also fixes the drawn width"),
    (decimals, "Decimals", "Digits after the decimal point"),
    (lit_color, "Lit Color", "Colour of the driven segments"),
    (ghost_color, "Ghost Color", "Colour of the undriven segments"),
    (show_ghosts, "Show Ghosts", "Draw the undriven segments behind the value"),
    (caption, "Caption", "Small label drawn beside the value"),
    (caption_position, "Caption Position", "right of the value, or above it"),
    (caption_color, "Caption Color", "Colour of the caption")
)

// `digits` sizes the ghost string that is built on every config change, and
// `decimals` reaches QString::number() as a precision. Both are unbounded in
// YAML and both produce a readout drawn far outside its own widget.
inline std::vector<std::string> validate(SegmentReadoutConfig_t& cfg)
{
    std::vector<std::string> notes;
    dashboard::limits::clampInto<uint16_t>(cfg.digits, 1u, 16u, "digits", notes);
    dashboard::limits::clampInto<uint16_t>(cfg.decimals, 0u, 6u, "decimals", notes);
    return notes;
}

#endif // SEGMENT_READOUT_CONFIG_H
