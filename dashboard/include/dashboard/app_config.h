#ifndef APP_CONFIG_H_
#define APP_CONFIG_H_

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "reflection/reflection.h"

//#include "carplay/carplay_widget.h"
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

REFLECT_ENUM(widget_type_t,
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
    //carplay,
    unknown
)

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
        //CarPlayWidget::config_t,
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

REFLECT_STRUCT(window_config_t,
    (std::string, name, ""),
    (uint16_t, width, 800),
    (uint16_t, height, 480),
    (std::string, background_color, "#000000"),
    (std::vector<widget_config_t>, widgets, {})
)

REFLECT_STRUCT(app_config_t,
    (std::vector<window_config_t>, windows, {})
)


std::optional<app_config_t> load_app_config(const std::string& config_filepath);


#endif  // APP_CONFIG_H_
