#ifndef VALUE_READOUT_CONFIG_H
#define VALUE_READOUT_CONFIG_H

#include <cstdint>
#include <string>
#include <vector>
#include "helpers/color.h"
#include "pub_sub/schema_registry.h"
#include "reflection/reflection.h"
#include "dashboard/config_limits.h"

REFLECT_ENUM(ValueReadoutAlignment,
	left,
	right,
	center
)

// How the number is rendered. `lap_time` takes a value in seconds and prints
// it the way a timing screen does -- 2:24.50 -- which is what the MoTeC lap
// fields show and what a plain number cannot express.
REFLECT_ENUM(ValueReadoutFormat,
	number,
	lap_time
)

REFLECT_STRUCT(ValueReadoutConfig_t,
	(std::string, label_text, "Untitled"),
	(ValueReadoutAlignment, alignment, ValueReadoutAlignment::left),
	(std::string, zenoh_key, ""),
    (pub_sub::schema_type_t, schema_type, pub_sub::schema_type_t::VehicleSpeed),
	(std::string, value_expression, ""),

	(ValueReadoutFormat, format, ValueReadoutFormat::number),
	// Digits after the decimal point. Ignored by lap_time, which always shows
	// hundredths.
	(uint16_t, decimals, 0),
	// Printed immediately after the value, e.g. "C" or "psi".
	(std::string, units, ""),
	// Always show a leading + on a positive value. A gain/loss field is
	// meaningless without it.
	(bool, show_sign, false),

	(helpers::Color, label_color, "#FFA500"),
	(helpers::Color, value_color, "#FFFFFF"),
	// The MoTeC screens are set in an italic face throughout.
	(bool, italic, false)
)

REFLECT_METADATA(ValueReadoutConfig_t,
	(label_text, "Label", "Label text to display"),
	(alignment, "Text Alignment", "Horizontal alignment of the text"),
	(zenoh_key, "Zenoh Key", "Zenoh topic key to subscribe to"),
	(schema_type, "Schema Type", "Data schema type for the subscription"),
	(value_expression, "Value Expression", "Expression to extract/compute the value to display"),
	(format, "Format", "number, or lap_time to render seconds as m:ss.SS"),
	(decimals, "Decimals", "Digits after the decimal point (number format only)"),
	(units, "Units", "Suffix printed after the value"),
	(show_sign, "Show Sign", "Always print a leading + on positive values"),
	(label_color, "Label Color", "Color of the label text"),
	(value_color, "Value Color", "Color of the value text"),
	(italic, "Italic", "Render label and value in italics")
)

// `decimals` reaches QString::number() as a precision. Qt takes an int there and
// a large value produces a string hundreds of characters wide, which does not
// fail -- it just draws a readout that is entirely off the widget.
inline std::vector<std::string> validate(ValueReadoutConfig_t& cfg)
{
	std::vector<std::string> notes;
	dashboard::limits::clampInto<uint16_t>(cfg.decimals, 0u, 6u, "decimals", notes);
	return notes;
}

#endif // VALUE_READOUT_CONFIG_H
