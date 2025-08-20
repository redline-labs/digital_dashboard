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
#include "mercedes_190e_telltales/telltale.h"
#include "sparkline/sparkline.h"
#include "mercedes_190e_cluster_gauge/mercedes_190e_cluster_gauge.h"
#include "motec_c125_tachometer/motec_c125_tachometer.h"
#include "motec_cdl3_tachometer/motec_cdl3_tachometer.h"
#include "static_text/static_text.h"
#include "value_readout/value_readout.h"
#include "background_rect/background_rect.h"

enum class widget_type_t
{
    mercedes_190e_speedometer,
    mercedes_190e_tachometer,
    mercedes_190e_telltale,
    mercedes_190e_cluster_gauge,
    motec_c125_tachometer,
    motec_cdl3_tachometer,
    static_text,
    value_readout,
    background_rect,

    sparkline,

    carplay,

    unknown
};

constexpr std::string_view widget_type_to_string(widget_type_t type)
{
    switch (type)
    {
        case widget_type_t::mercedes_190e_speedometer:
            return "mercedes_190e_speedometer";

        case widget_type_t::mercedes_190e_tachometer:
            return "mercedes_190e_tachometer";

        case widget_type_t::mercedes_190e_telltale:
            return "mercedes_190e_telltale";

        case widget_type_t::mercedes_190e_cluster_gauge:
            return "mercedes_190e_cluster_gauge";

        case widget_type_t::motec_c125_tachometer:
            return "motec_c125_tachometer";

        case widget_type_t::motec_cdl3_tachometer:
            return "motec_cdl3_tachometer";

        case widget_type_t::static_text:
            return "static_text";

        case widget_type_t::value_readout:
            return "value_readout";

        case widget_type_t::background_rect:
            return "background_rect";

        case widget_type_t::sparkline:
            return "sparkline";

        case widget_type_t::carplay:
            return "carplay";

        case widget_type_t::unknown:
        default:
            return "unknown";
    }
}

struct widget_config_t {
    widget_config_t() :
        type{widget_type_t::unknown},
        x{0},
        y{0},
        width{100},
        height{100},
        config{}
    {}

    widget_type_t type;
    int16_t x;
    int16_t y;
    uint16_t width;
    uint16_t height;

    std::variant<
        Mercedes190ESpeedometer::config_t,
        CarPlayWidget::config_t,
        Mercedes190ETachometer::config_t,
        Mercedes190ETelltale::config_t,
        SparklineItem::config_t,
        Mercedes190EClusterGauge::config_t,
        MotecC125Tachometer::config_t,
        MotecCdl3Tachometer::config_t,
        StaticTextWidget::config_t,
        ValueReadoutWidget::config_t,
        BackgroundRectWidget::config_t> config;
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
