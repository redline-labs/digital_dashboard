#ifndef DASHBOARD_EDITOR_SELECTION_FRAME_H
#define DASHBOARD_EDITOR_SELECTION_FRAME_H

#include <QWidget>
#include <QRect>

#include "dashboard/app_config.h"

// Widget headers for config types
#include "static_text/config.h"
#include "background_rect/config.h"
#include "mercedes_190e_cluster_gauge/config.h"
#include "mercedes_190e_speedometer/config.h"
#include "mercedes_190e_tachometer/config.h"
#include "motec_c125_tachometer/config.h"
#include "motec_cdl3_tachometer/config.h"
#include "sparkline/config.h"
#include "value_readout/config.h"
//#include "carplay/config.h"
#include "mercedes_190e_telltales/config.h"

#include <spdlog/spdlog.h>
#include "reflection/reflection.h"
#include "editor/widget_registry.h"

class SelectionFrame : public QWidget
{
    Q_OBJECT

public:
    constexpr static int kGrabHandleSizePx = 12;

    explicit SelectionFrame(widget_type_t type, QWidget* parent = nullptr);

    widget_type_t type() const { return type_; }
    QWidget* child() const { return child_; }

    void setSelected(bool on);
    bool isSelected() const { return selected_; }

    // Editor mode: when true, this frame captures interactions; when false, pass through to child
    void setEditorModeCapture(bool on);

    enum class Handle { None, Move, ResizeTL, ResizeTR, ResizeBL, ResizeBR };
    // Hit-test using canvas coordinates (parent space)
    Handle hitTestCanvasPos(const QPoint& canvasPos) const;

    template <typename Config>
    void applyConfig(const Config& cfg)
    {
        using traits = widget_registry::widget_traits<Config>;
        using widget_t = typename traits::widget_t;
        static_assert(!std::is_void_v<widget_t>, "Unsupported config type");

        // Reject mismatched config for this frame
        if (type_ != traits::type)
        {
            SPDLOG_ERROR("Type mismatch, expected '{}', received '{}'.", reflection::enum_to_string(type_), reflection::enum_to_string(traits::type));
            return;
        }

        // Rebuild child
        setChild(new widget_t(cfg, nullptr));
    }

    // Convert the contained widget back into a widget_config_t for saving
    widget_config_t toWidgetConfig(const QRect& frameRect) const
    {
        widget_config_t wc;
        wc.type = type_;
        wc.x = static_cast<int16_t>(frameRect.x());
        wc.y = static_cast<int16_t>(frameRect.y());
        wc.width = static_cast<uint16_t>(frameRect.width());
        wc.height = static_cast<uint16_t>(frameRect.height());

        if (!child_)
        {
            return wc;
        }

        switch (type_)
        {
            case widget_type_t::static_text:
                wc.config = static_cast<StaticTextWidget*>(child_)->getConfig();
                break;
            case widget_type_t::background_rect:
                wc.config = static_cast<BackgroundRectWidget*>(child_)->getConfig();
                break;
            case widget_type_t::mercedes_190e_cluster_gauge:
                wc.config = static_cast<Mercedes190EClusterGauge*>(child_)->getConfig();
                break;
            case widget_type_t::mercedes_190e_speedometer:
                wc.config = static_cast<Mercedes190ESpeedometer*>(child_)->getConfig();
                break;
            case widget_type_t::mercedes_190e_tachometer:
                wc.config = static_cast<Mercedes190ETachometer*>(child_)->getConfig();
                break;
            case widget_type_t::motec_c125_tachometer:
                wc.config = static_cast<MotecC125Tachometer*>(child_)->getConfig();
                break;
            case widget_type_t::motec_cdl3_tachometer:
                wc.config = static_cast<MotecCdl3Tachometer*>(child_)->getConfig();
                break;
            case widget_type_t::sparkline:
                wc.config = static_cast<SparklineItem*>(child_)->getConfig();
                break;
            case widget_type_t::value_readout:
                wc.config = static_cast<ValueReadoutWidget*>(child_)->getConfig();
                break;
            case widget_type_t::mercedes_190e_telltale:
                wc.config = static_cast<Mercedes190ETelltale*>(child_)->getConfig();
                break;
            case widget_type_t::carplay:
                wc.config = static_cast<CarPlayWidget*>(child_)->getConfig();
                break;
            case widget_type_t::unknown:
            default:
                break;
        }

        return wc;
    }

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    widget_type_t type_;
    QWidget* child_ = nullptr;
    bool selected_ = false;
    bool editorMode_ = true;
    QWidget* overlay_ = nullptr; // draws selection chrome above child

    void setChild(QWidget* newChild);
};

#endif // DASHBOARD_EDITOR_SELECTION_FRAME_H


