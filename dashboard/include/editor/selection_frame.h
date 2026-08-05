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

    // Builds the default-configured child, if applyConfig() has not already
    // built a configured one. A frame is constructed childless precisely so that
    // a caller holding a config does not pay for a widget it is about to
    // replace; every caller must therefore end up calling one or the other.
    void ensureChild();

    // Optional stable handle, round-tripped through the YAML `id:` key. Set on
    // the frame rather than the child so it survives applyConfig() rebuilding
    // the child widget.
    const std::string& id() const { return id_; }
    void setId(std::string id);

    void setSelected(bool on);
    bool isSelected() const { return selected_; }

    // Editor mode: when true, this frame captures interactions; when false, pass through to child
    void setEditorModeCapture(bool on);

    enum class Handle { None, Move, ResizeTL, ResizeTR, ResizeBL, ResizeBR };
    // Hit-test using canvas coordinates (parent space)
    Handle hitTestCanvasPos(const QPoint& canvasPos) const;

    // The configuration this frame was given, verbatim.
    //
    // Authoritative, and deliberately not read back off the live widget. The
    // preview is built through widget_factory, which clamps a config into
    // something drawable before the widget sees it, so getConfig() returns the
    // clamped value rather than the configured one -- exporting from there would
    // rewrite the user's file with whatever the clamp produced. Keeping the
    // original here means the editor previews exactly what the dashboard will
    // draw while still saving exactly what was asked for.
    const widget_config_variant_t& config() const { return config_; }

    // Returns false if `cfg` is not for this frame's type. Callers must check:
    // the agent's set_config used to report `applied` unconditionally, so a
    // rejected config came back as a success.
    template <typename Config>
    [[nodiscard]] bool applyConfig(const Config& cfg)
    {
        using traits = widget_registry::config_traits<Config>;
        using widget_t = typename traits::widget_t;
        static_assert(!std::is_void_v<widget_t>, "Unsupported config type");

        // Reject mismatched config for this frame
        if (type_ != traits::type)
        {
            SPDLOG_ERROR("Type mismatch, expected '{}', received '{}'.", reflection::enum_to_string(type_), reflection::enum_to_string(traits::type));
            return false;
        }

        config_ = cfg;
        rebuildChild();
        return true;
    }

    // The frame as a config entry, for saving.
    //
    // Geometry comes from the caller because the widget owns it: Qt moves and
    // resizes the frame directly during a drag, and a second copy here would be
    // one more thing to keep in step. A cached `position` field lived on
    // Canvas::Item once and had already gone stale by the time it was removed --
    // written on creation, never updated by a drag, editor.move or
    // editor.resize. Not worth re-introducing.
    //
    // The configuration, by contrast, comes from this frame rather than from the
    // live widget. See config().
    widget_config_t toWidgetConfig(const QRect& frameRect) const
    {
        widget_config_t wc;
        wc.type = type_;
        wc.id = id_;
        wc.x = static_cast<int16_t>(frameRect.x());
        wc.y = static_cast<int16_t>(frameRect.y());
        wc.width = static_cast<uint16_t>(frameRect.width());
        wc.height = static_cast<uint16_t>(frameRect.height());
        wc.config = config_;
        return wc;
    }

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    widget_type_t type_;
    std::string id_;
    widget_config_variant_t config_{std::monostate{}};
    QWidget* child_ = nullptr;
    bool selected_ = false;
    bool editorMode_ = true;
    QWidget* overlay_ = nullptr; // draws selection chrome above child

    // Builds the preview widget from config_ and swaps it in. Goes through
    // widget_factory so the editor clamps exactly as the dashboard does.
    void rebuildChild();

    void setChild(QWidget* newChild);
};

#endif // DASHBOARD_EDITOR_SELECTION_FRAME_H


