#ifndef VALUE_READOUT_CONFIG_H
#define VALUE_READOUT_CONFIG_H

#include <cstdint>
#include <string>
#include "pub_sub/schema_registry.h"
#include "reflection/reflection.h"

REFLECT_ENUM(ValueReadoutAlignment,
	left,
	right,
	center
)

REFLECT_STRUCT(ValueReadoutConfig_t,
	(std::string, label_text, "Untitled"),
	(ValueReadoutAlignment, alignment, ValueReadoutAlignment::left),
	(std::string, zenoh_key, ""),
    (pub_sub::schema_type_t, schema_type, pub_sub::schema_type_t::VehicleSpeed),
	(std::string, value_expression, "")
)

REFLECT_METADATA(ValueReadoutConfig_t,
	(label_text, "Label", "Label text to display"),
	(alignment, "Text Alignment", "Horizontal alignment of the text"),
	(zenoh_key, "Zenoh Key", "Zenoh topic key to subscribe to"),
	(schema_type, "Schema Type", "Data schema type for the subscription"),
	(value_expression, "Value Expression", "Expression to extract/compute the value to display")
)

#endif // VALUE_READOUT_CONFIG_H
