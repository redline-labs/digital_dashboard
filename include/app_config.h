#ifndef APP_CONFIG_H_
#define APP_CONFIG_H_

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "carplay/carplay_widget.h"
#include "mercedes_190e_speedometer/mercedes_190e_speedometer.h"
#include "mercedes_190e_tachometer/mercedes_190e_tachometer.h"
#include "mercedes_190e_telltales/battery_telltale.h"
#include "sparkline/sparkline.h"
#include "mercedes_190e_cluster_gauge/mercedes_190e_cluster_gauge.h"

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
        Mercedes190ESpeedometer::config_t,
        CarPlayWidget::config_t,
        Mercedes190ETachometer::config_t,
        Mercedes190EBatteryTelltale::config_t,
        SparklineItem::config_t,
        Mercedes190EClusterGauge::config_t> config;
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
