#ifndef APP_CONFIG_H_
#define APP_CONFIG_H_

#include <cstdint>
#include <string>
#include <string_view>
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
        zenoh_key{},
        config{}
    {}

    std::string type;
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
    std::string zenoh_key;  // Optional Zenoh subscription key for real-time data

    std::variant<
        speedometer_config_t,
        carplay_config_t,
        tachometer_config_t,
        battery_telltale_config_t,
        sparkline_config_t,
        cluster_gauge_config_t> config;
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


app_config_t load_app_config(const std::string& config_filepath);


#endif  // APP_CONFIG_H_
