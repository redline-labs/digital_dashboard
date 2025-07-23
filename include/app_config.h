#ifndef APP_CONFIG_H_
#define APP_CONFIG_H_

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "carplay/config.h"
#include "mercedes_190e_speedometer/config.h"
#include "mercedes_190e_tachometer/config.h"
#include "mercedes_190e_telltales/config.h"
#include "sparkline/config.h"
#include "mercedes_190e_cluster_gauge/config.h"

struct widget_config_t {
    widget_config_t() :
        type{},
        x{0},
        y{0},
        width{100},
        height{100},
        config{}
    {}

    std::string type;
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;

    std::variant<
        Mercedes190ESpeedometerConfig_t,
        CarplayConfig_t,
        Mercedes190ETachometerConfig_t,
        Mercedes190EBatteryTelltaleConfig_t,
        SparklineConfig_t,
        Mercedes190EClusterGaugeConfig_t> config;
};

struct window_config_t {
    window_config_t() :
        name{},
        width{800},
        height{480},
        background_color{"#000000"},
        widgets{}
    {}

    std::string name;
    uint16_t width;
    uint16_t height;
    std::string background_color;  // Window background color in hex format (#RRGGBB)
    std::vector<widget_config_t> widgets;
};

struct app_config_t {

    app_config_t() :
        windows{}
    {}

    // Window layout configuration - support multiple windows
    std::vector<window_config_t> windows;
};


std::optional<app_config_t> load_app_config(const std::string& config_filepath);


#endif  // APP_CONFIG_H_
