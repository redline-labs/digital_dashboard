#ifndef VALUE_READOUT_CONFIG_H
#define VALUE_READOUT_CONFIG_H

#include <cstdint>
#include <string>

enum class ValueReadoutAlignment {
	Left,
	Right,
	Center
};

struct ValueReadoutConfig_t {
	ValueReadoutConfig_t() :
		label_text{"WATER TMP"},
		alignment{ValueReadoutAlignment::Right},
		zenoh_key{},
		schema_type{"EngineTemperature"},
		value_expression{"temperatureCelsius"}
	{}

	std::string label_text;   // Orange label text (e.g., WATER TMP)
	ValueReadoutAlignment alignment; // Left, Right, or Center

	// Data source
	std::string zenoh_key;    // Topic/key for subscription
	std::string schema_type;  // Cap'n Proto schema type name
	std::string value_expression; // Expression to compute display value
};

#endif // VALUE_READOUT_CONFIG_H
