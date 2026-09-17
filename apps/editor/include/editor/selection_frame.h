#ifndef DASHBOARD_EDITOR_SELECTION_FRAME_H
#define DASHBOARD_EDITOR_SELECTION_FRAME_H

#include <QPointer>
#include <QWidget>
#include <QRect>

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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
#include "dashboard/widget_registry.h"

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
    // Hit-test a point in this frame's own coordinates. The caller maps: a frame
    // on a page_stack's page is not a child of the canvas, so "parent space" is
    // not canvas space.
    Handle hitTestLocal(const QPoint& localPos) const;

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

    // ------------------------------------------------------------- page_stack
    //
    // A page_stack's frame holds its pages as live frames: one SelectionFrame per
    // widget on each page, parented to this frame and placed in its coordinates,
    // which are exactly the stack-relative coordinates the config stores. Only the
    // page being previewed is shown. The frames are the document, as everywhere
    // else in the editor -- pages() reads the config back off them.
    bool isContainer() const { return type_ == widget_type_t::page_stack; }

    // The pages as a config: names, in_cycle, and each page's widgets read off
    // its frames. Empty for anything but a page_stack.
    std::vector<widget_page_t> pages() const;

    // The object names of each page's widgets, page by page. They are not in the
    // config -- a widget with no id is named from its position -- so they ride
    // with history snapshots beside it.
    std::vector<std::vector<QString>> pageChildNames() const;

    // Brings the pages in line with `pages`, reusing a live frame wherever
    // `names` finds one of the same type, so an undo that moved one widget does
    // not rebuild its neighbours (a CarPlay preview among them). `names` may be
    // empty or the wrong shape; the frames are then named by the dashboard's rule.
    void applyPages(const std::vector<widget_page_t>& pages,
                    const std::vector<std::vector<QString>>& names = {});

    std::size_t pageCount() const { return pageSlots_.size(); }
    const std::string& pageName(std::size_t page) const { return pageSlots_[page].name; }
    bool pageInCycle(std::size_t page) const { return pageSlots_[page].in_cycle; }
    std::vector<SelectionFrame*> pageFrames(std::size_t page) const;
    std::optional<std::size_t> pageIndex(const std::string& name) const;

    // Which page the editor previews. Not saved; the dashboard starts on
    // default_page whatever the editor last showed.
    std::size_t shownPage() const { return shownPage_; }
    void showPage(std::size_t page);

    // Adds a widget with its own defaults to `page`, at `localPos`. Null for a
    // page_stack: one cannot sit on another's page. The caller opens the history
    // entry.
    SelectionFrame* addPageChild(std::size_t page, widget_type_t type, const QPoint& localPos,
                                 const QSize& size, bool editorMode);

    // Removes a widget from its page and schedules it for deletion.
    bool removePageChild(SelectionFrame* child);

    // Where a widget sits: (page, position on the page). nullopt if it is not
    // one of this stack's.
    std::optional<std::pair<std::size_t, std::size_t>> locateChild(const SelectionFrame* child) const;

    // The stack this frame is on, if it is on one.
    SelectionFrame* containerFrame() const;

    // Drawn while the canvas is editing inside this stack: a different outline,
    // and the name of the page being previewed.
    void setScopeActive(bool on);
    bool isScopeActive() const { return scopeActive_; }

    // Replaces the stored configuration and rebuilds the preview from it.
    //
    // The variant form of applyConfig(), for the one caller that already holds a
    // widget_config_variant_t rather than a concrete config type: restoring a
    // history entry, which has just decided this frame's configuration actually
    // differs and so has to be rebuilt. A type mismatch between the variant's
    // alternative and this frame's type is caught and logged by widget_factory.
    void applyStoredConfig(const widget_config_variant_t& config)
    {
        config_ = config;
        rebuildChild();
    }

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
        wc.pages = pages();
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

    struct PageSlot
    {
        std::string name;
        bool in_cycle = true;
        std::vector<QPointer<SelectionFrame>> frames;
    };
    std::vector<PageSlot> pageSlots_;
    std::size_t shownPage_ = 0;
    // Monotonic, like Canvas::nextNameIndex_: a derived name is never reused.
    std::size_t nextChildIndex_ = 0;
    bool scopeActive_ = false;

    SelectionFrame* buildPageChild(const widget_config_t& cfg, const QString& name, bool editorMode);
    // Stacking inside the frame: preview widget at the bottom, then each page's
    // frames in document order, selection chrome on top.
    void restack();
    bool selected_ = false;
    bool editorMode_ = true;
    QWidget* overlay_ = nullptr; // draws selection chrome above child

    // Builds the preview widget from config_ and swaps it in. Goes through
    // widget_factory so the editor clamps exactly as the dashboard does.
    void rebuildChild();

    void setChild(QWidget* newChild);

    // Tells a page_stack preview which pages to name in its outline.
    void labelPages();
    // Shows the shown page's frames and hides the rest.
    void applyPageVisibility();
};

#endif // DASHBOARD_EDITOR_SELECTION_FRAME_H


