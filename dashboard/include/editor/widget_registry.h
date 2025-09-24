#ifndef DASHBOARD_EDITOR_WIDGET_REGISTRY_H
#define DASHBOARD_EDITOR_WIDGET_REGISTRY_H

#include <array>
#include <string_view>

#include <QString>
#include <QWidget>

#include "dashboard/app_config.h"
#include "carplay/carplay_widget.h"

// Central registry for editor: single source of truth for
// - available widget types and display labels
// - enum-string conversion used in drag & drop
// - creation of widgets with reasonable default configs
// - mapping from QWidget* instance to widget_type_t
namespace widget_registry
{

struct WidgetInfo
{
	widget_type_t type;
	const std::string_view label;         // user-visible label
	const std::string_view enum_name;     // stable enum string name (for DnD / config)
};

constexpr std::array<WidgetInfo, 13> kAllWidgets =
{{
	{widget_type_t::static_text, "Static Text", StaticTextWidget::kWidgetName},
	{widget_type_t::value_readout, "Value Readout", ValueReadoutWidget::kWidgetName},
	{widget_type_t::mercedes_190e_speedometer, "Mercedes 190E Speedometer", Mercedes190ESpeedometer::kWidgetName},
	{widget_type_t::mercedes_190e_tachometer, "Mercedes 190E Tachometer", Mercedes190ETachometer::kWidgetName},
	{widget_type_t::mercedes_190e_cluster_gauge, "Mercedes 190E Cluster Gauge", Mercedes190EClusterGauge::kWidgetName},
	{widget_type_t::sparkline, "Sparkline", SparklineItem::kWidgetName},
	{widget_type_t::background_rect, "Background Rect", BackgroundRectWidget::kWidgetName},
	{widget_type_t::mercedes_190e_telltale, "Mercedes 190E Telltale", Mercedes190ETelltale::kWidgetName},
	{widget_type_t::motec_c125_tachometer, "MoTeC C125 Tachometer", MotecC125Tachometer::kWidgetName},
    {widget_type_t::motec_cdl3_tachometer, "MoTeC CDL3 Tachometer", MotecCdl3Tachometer::kWidgetName},
    {widget_type_t::carplay, "CarPlay", CarPlayWidget::kWidgetName}
}};

template <typename Config> struct widget_traits;

template <>
struct widget_traits<StaticTextWidget::config_t>
{
	static constexpr widget_type_t type = widget_type_t::static_text;
	using widget_t = StaticTextWidget;
};

template <>
struct widget_traits<BackgroundRectWidget::config_t>
{
	static constexpr widget_type_t type = widget_type_t::background_rect;
	using widget_t = BackgroundRectWidget;
};

template <>
struct widget_traits<Mercedes190EClusterGauge::config_t>
{
	static constexpr widget_type_t type = widget_type_t::mercedes_190e_cluster_gauge;
	using widget_t = Mercedes190EClusterGauge;
};

template <>
struct widget_traits<Mercedes190ESpeedometer::config_t>
{
	static constexpr widget_type_t type = widget_type_t::mercedes_190e_speedometer;
	using widget_t = Mercedes190ESpeedometer;
};

template <>
struct widget_traits<Mercedes190ETachometer::config_t>
{
	static constexpr widget_type_t type = widget_type_t::mercedes_190e_tachometer;
	using widget_t = Mercedes190ETachometer;
};

template <>
struct widget_traits<MotecC125Tachometer::config_t>
{
	static constexpr widget_type_t type = widget_type_t::motec_c125_tachometer;
	using widget_t = MotecC125Tachometer;
};

template <>
struct widget_traits<MotecCdl3Tachometer::config_t>
{
	static constexpr widget_type_t type = widget_type_t::motec_cdl3_tachometer;
	using widget_t = MotecCdl3Tachometer;
};

template <>
struct widget_traits<SparklineItem::config_t>
{
	static constexpr widget_type_t type = widget_type_t::sparkline;
	using widget_t = SparklineItem;
};

template <>
struct widget_traits<ValueReadoutWidget::config_t>
{
	static constexpr widget_type_t type = widget_type_t::value_readout;
	using widget_t = ValueReadoutWidget;
};

template <>
struct widget_traits<CarPlayWidget::config_t>
{
    static constexpr widget_type_t type = widget_type_t::carplay;
    using widget_t = CarPlayWidget;
};

template <>
struct widget_traits<Mercedes190ETelltale::config_t>
{
	static constexpr widget_type_t type = widget_type_t::mercedes_190e_telltale;
	using widget_t = Mercedes190ETelltale;
};


inline QWidget* instantiateWidget(widget_type_t type, QWidget* parent)
{
	QWidget* w = nullptr;
	switch (type)
	{
		case widget_type_t::static_text: {
			StaticTextConfig_t cfg;
			w = new StaticTextWidget(cfg, parent);
			break;
		}
		case widget_type_t::value_readout: {
			ValueReadoutConfig_t cfg;
			w = new ValueReadoutWidget(cfg, parent);
			break;
		}
		case widget_type_t::mercedes_190e_speedometer: {
			Mercedes190ESpeedometerConfig_t cfg;
			w = new Mercedes190ESpeedometer(cfg, parent);
			break;
		}
		case widget_type_t::mercedes_190e_tachometer: {
			Mercedes190ETachometerConfig_t cfg;
			w = new Mercedes190ETachometer(cfg, parent);
			break;
		}
		case widget_type_t::mercedes_190e_cluster_gauge: {
			Mercedes190EClusterGaugeConfig_t cfg;
			w = new Mercedes190EClusterGauge(cfg, parent);
			break;
		}
		case widget_type_t::sparkline: {
			SparklineConfig_t cfg;
			w = new SparklineItem(cfg, parent);
			break;
		}
		case widget_type_t::background_rect: {
			BackgroundRectConfig_t cfg;
			w = new BackgroundRectWidget(cfg, parent);
			break;
		}
		case widget_type_t::mercedes_190e_telltale: {
			Mercedes190ETelltaleConfig_t cfg;
			w = new Mercedes190ETelltale(cfg, parent);
			break;
		}
		case widget_type_t::motec_c125_tachometer: {
			MotecC125TachometerConfig_t cfg;
			w = new MotecC125Tachometer(cfg, parent);
			break;
		}
		case widget_type_t::motec_cdl3_tachometer: {
			MotecCdl3TachometerConfig_t cfg;
			w = new MotecCdl3Tachometer(cfg, parent);
			break;
		}
        case widget_type_t::carplay: {
            CarplayConfig_t cfg;
            w = new CarPlayWidget(cfg, parent);
            break;
        }
		case widget_type_t::unknown:
		default:
			w = nullptr;
			break;
	}
	return w;
}

} // namespace widget_registry

#endif // DASHBOARD_EDITOR_WIDGET_REGISTRY_H


