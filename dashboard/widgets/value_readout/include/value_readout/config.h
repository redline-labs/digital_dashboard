#ifndef VALUE_READOUT_CONFIG_H
#define VALUE_READOUT_CONFIG_H

#include <cstdint>
#include <string>
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
	(std::string, schema_type, ""),
	(std::string, value_expression, "")
)

#endif // VALUE_READOUT_CONFIG_H
