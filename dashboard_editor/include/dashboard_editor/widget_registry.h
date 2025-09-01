#ifndef DASHBOARD_EDITOR_WIDGET_REGISTRY_H
#define DASHBOARD_EDITOR_WIDGET_REGISTRY_H

#include <array>
#include <string_view>

#include <QString>
#include <QWidget>

#include "app_config.h"

// Central registry for editor: single source of truth for
// - available widget types and display labels
// - enum-string conversion used in drag & drop
// - creation of widgets with reasonable default configs
// - mapping from QWidget* instance to widget_type_t
namespace widget_registry
{

struct WidgetInfo {
	widget_type_t type;
	const std::string_view label;         // user-visible label
	const std::string_view enum_name;     // stable enum string name (for DnD / config)
};

constexpr std::array<WidgetInfo, 12> kAllWidgets =
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

inline QWidget* instantiateWidget(widget_type_t type, QWidget* parent)
{
	QWidget* w = nullptr;
	switch (type)
	{
		case widget_type_t::static_text: {
			StaticTextConfig_t cfg;
			cfg.text = "Text";
			cfg.font = "Futura";
			cfg.font_size = 18;
			cfg.color = "#FFFFFF";
			w = new StaticTextWidget(cfg, parent);
			break;
		}
		case widget_type_t::value_readout: {
			ValueReadoutConfig_t cfg;
			cfg.label_text = "WATER TMP";
			cfg.alignment = ValueReadoutAlignment::left;
			w = new ValueReadoutWidget(cfg, parent);
			break;
		}
		case widget_type_t::mercedes_190e_speedometer: {
			Mercedes190ESpeedometerConfig_t cfg;
			cfg.max_speed = 125;
			w = new Mercedes190ESpeedometer(cfg, parent);
			break;
		}
		case widget_type_t::mercedes_190e_tachometer: {
			Mercedes190ETachometerConfig_t cfg;
			cfg.max_rpm = 7000;
			cfg.redline_rpm = 6000;
			cfg.show_clock = false;
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
			cfg.units = "rpm";
			w = new SparklineItem(cfg, parent);
			break;
		}
		case widget_type_t::background_rect: {
			BackgroundRectConfig_t cfg;
			cfg.colors = {"#202020", "#101010"};
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
			w = new CarPlayWidget(cfg);
			break;
		}
		case widget_type_t::unknown:
		default:
			w = nullptr;
			break;
	}
	if (w)
	{
		w->setProperty("widgetType", static_cast<int>(type));
	}
	return w;
}

inline widget_type_t widgetTypeFor(QWidget* w)
{
	if (!w) return widget_type_t::unknown;
	const QVariant v = w->property("widgetType");
	if (v.isValid())
	{
		return static_cast<widget_type_t>(v.toInt());
	}
	return widget_type_t::unknown;
}

} // namespace widget_registry

#endif // DASHBOARD_EDITOR_WIDGET_REGISTRY_H


