#include "editor/selection_frame.h"
#include "dashboard/widget_registry.h"

#include "dashboard/widget_factory.h"

#include "dashboard/widget_identity.h"

#include <QPainter>
#include <QEvent>

#include <algorithm>
#include <map>

namespace
{
    // Colors for selection chrome
    // kSelectedOutlineColor: Active selection outline color (medium blue)
    // kUnselectedOutlineColor: Non-selected frame outline color in editor mode (dark gray)
    constexpr QColor kSelectedOutlineColor(0, 122, 255);
    constexpr QColor kUnselectedOutlineColor(80, 80, 80);
    // kScopeOutlineColor: the page_stack being edited into (amber)
    constexpr QColor kScopeOutlineColor(255, 176, 0);
}

SelectionFrame::SelectionFrame(widget_type_t type, QWidget* parent)
    : QWidget(parent), type_(type), child_(nullptr)
{
    setAttribute(Qt::WA_TransparentForMouseEvents, false);
    setAttribute(Qt::WA_NoSystemBackground);

    // The child is deliberately NOT built here.
    //
    // It used to be, with a default config, and every caller that has a config
    // block then called applyConfig() -- which replaces the child. So each load,
    // undo and redo built every widget twice and threw the first one away:
    // measured at exactly 2.00 CarPlayWidget constructions per load. That is not
    // a cheap object to build and discard. It declares three zenoh
    // subscriptions and two publishers, and if a packet happens to land in the
    // few milliseconds it is alive it also opens an H.264 decoder and a
    // CoreAudio sink, on the real output device.
    //
    // It was not only waste. The discarded widget is torn down at the same
    // moment its replacement is starting up, which is exactly the overlap the
    // zenoh-callback/GUI-thread deadlock needed (see
    // CarPlayWidget::requestAudioSink). Undo manufactured that race on every
    // press.
    //
    // Callers with no config to apply -- a palette drop -- ask for the default
    // child explicitly with ensureChild().
    overlay_ = new QWidget(this);
    overlay_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    overlay_->setAttribute(Qt::WA_NoSystemBackground, true);
    overlay_->resize(size());
    overlay_->raise();
    overlay_->installEventFilter(this);
}

void SelectionFrame::setId(std::string id)
{
    id_ = std::move(id);

    // Mirror onto objectName so the agent control interface addresses a frame in
    // the editor by exactly the string the dashboard will use for the same
    // widget once the config is saved and loaded.
    //
    // An empty id deliberately leaves objectName alone: the caller then applies
    // the derived "<type>#<index>" fallback, and clearing it here would undo
    // that. Only a real id overrides the derived name.
    if (!id_.empty())
    {
        setObjectName(QString::fromStdString(id_));
    }
}

std::vector<widget_page_t> SelectionFrame::pages() const
{
    std::vector<widget_page_t> out;
    out.reserve(pageSlots_.size());
    for (const PageSlot& slot : pageSlots_)
    {
        widget_page_t page;
        page.name = slot.name;
        page.in_cycle = slot.in_cycle;
        for (const auto& frame : slot.frames)
        {
            if (frame)
            {
                page.widgets.push_back(frame->toWidgetConfig(QRect(frame->pos(), frame->size())));
            }
        }
        out.push_back(std::move(page));
    }
    return out;
}

std::vector<std::vector<QString>> SelectionFrame::pageChildNames() const
{
    std::vector<std::vector<QString>> out;
    out.reserve(pageSlots_.size());
    for (const PageSlot& slot : pageSlots_)
    {
        std::vector<QString> names;
        for (const auto& frame : slot.frames)
        {
            if (frame)
            {
                names.push_back(frame->objectName());
            }
        }
        out.push_back(std::move(names));
    }
    return out;
}

SelectionFrame* SelectionFrame::buildPageChild(const widget_config_t& cfg, const QString& name, bool editorMode)
{
    if (cfg.type == widget_type_t::unknown || cfg.type == widget_type_t::page_stack)
    {
        SPDLOG_WARN("'{}': a {} cannot be on a page; left out.", objectName().toStdString(),
                    reflection::enum_to_string(cfg.type));
        return nullptr;
    }

    auto* frame = new SelectionFrame(cfg.type, this);
    frame->setId(cfg.id);
    frame->setObjectName(name);
    frame->applyStoredConfig(cfg.config);
    frame->ensureChild();
    frame->move(cfg.x, cfg.y);
    frame->resize(cfg.width, cfg.height);
    frame->setEditorModeCapture(editorMode);
    return frame;
}

void SelectionFrame::applyPages(const std::vector<widget_page_t>& pages,
                                const std::vector<std::vector<QString>>& names)
{
    if (!isContainer())
    {
        return;
    }

    // Every live frame, by name, wherever it is now: a widget moved to another
    // page is still the same widget.
    std::map<QString, SelectionFrame*> live;
    for (const PageSlot& slot : pageSlots_)
    {
        for (const auto& frame : slot.frames)
        {
            if (frame)
            {
                live.emplace(frame->objectName(), frame.data());
            }
        }
    }

    bool names_fit = names.size() == pages.size();
    for (std::size_t p = 0; names_fit && p < pages.size(); ++p)
    {
        names_fit = names[p].size() == pages[p].widgets.size();
    }

    std::vector<PageSlot> rebuilt;
    rebuilt.reserve(pages.size());
    std::size_t largest = 0;
    for (std::size_t p = 0; p < pages.size(); ++p)
    {
        const widget_page_t& page = pages[p];
        PageSlot slot{page.name, page.in_cycle, {}};
        largest = std::max(largest, page.widgets.size());

        for (std::size_t w = 0; w < page.widgets.size(); ++w)
        {
            const widget_config_t& cfg = page.widgets[w];
            const QString name = names_fit ? names[p][w]
                                           : dashboard::childWidgetObjectName(objectName(), page.name, cfg, w);

            SelectionFrame* frame = nullptr;
            if (const auto it = live.find(name); it != live.end() && it->second->type() == cfg.type)
            {
                frame = it->second;
                live.erase(it);
                if (!(frame->config() == cfg.config))
                {
                    frame->applyStoredConfig(cfg.config);
                }
                frame->setId(cfg.id);
                frame->setObjectName(name);
                const QRect target(cfg.x, cfg.y, cfg.width, cfg.height);
                if (QRect(frame->pos(), frame->size()) != target)
                {
                    frame->move(target.topLeft());
                    frame->resize(target.size());
                }
            }
            else
            {
                frame = buildPageChild(cfg, name, editorMode_);
            }

            if (frame != nullptr)
            {
                slot.frames.push_back(frame);
            }
        }
        rebuilt.push_back(std::move(slot));
    }

    // Whatever was not claimed is not in the new pages. Hidden now, deleted
    // later: deleteLater leaves it in the tree until the event loop runs.
    for (const auto& [name, frame] : live)
    {
        frame->hide();
        frame->deleteLater();
    }

    pageSlots_ = std::move(rebuilt);
    nextChildIndex_ = std::max(nextChildIndex_, largest);
    if (shownPage_ >= pageSlots_.size())
    {
        shownPage_ = pageSlots_.empty() ? 0 : pageSlots_.size() - 1;
    }
    restack();
    applyPageVisibility();
    labelPages();
}

std::vector<SelectionFrame*> SelectionFrame::pageFrames(std::size_t page) const
{
    std::vector<SelectionFrame*> out;
    if (page >= pageSlots_.size())
    {
        return out;
    }
    for (const auto& frame : pageSlots_[page].frames)
    {
        if (frame)
        {
            out.push_back(frame);
        }
    }
    return out;
}

std::optional<std::size_t> SelectionFrame::pageIndex(const std::string& name) const
{
    for (std::size_t i = 0; i < pageSlots_.size(); ++i)
    {
        if (pageSlots_[i].name == name)
        {
            return i;
        }
    }
    return std::nullopt;
}

void SelectionFrame::showPage(std::size_t page)
{
    if (page >= pageSlots_.size() || page == shownPage_)
    {
        return;
    }
    shownPage_ = page;
    applyPageVisibility();
    labelPages();
    if (overlay_)
    {
        overlay_->update();
    }
}

void SelectionFrame::applyPageVisibility()
{
    for (std::size_t p = 0; p < pageSlots_.size(); ++p)
    {
        for (const auto& frame : pageSlots_[p].frames)
        {
            if (frame)
            {
                frame->setVisible(p == shownPage_);
            }
        }
    }
}

SelectionFrame* SelectionFrame::addPageChild(std::size_t page, widget_type_t type, const QPoint& localPos,
                                             const QSize& size, bool editorMode)
{
    if (!isContainer() || page >= pageSlots_.size() || type == widget_type_t::unknown ||
        type == widget_type_t::page_stack)
    {
        return nullptr;
    }

    widget_config_t cfg;
    cfg.type = type;
    cfg.config = default_widget_config(type);
    cfg.x = static_cast<int16_t>(localPos.x());
    cfg.y = static_cast<int16_t>(localPos.y());
    const QSize chosen = size.isValid() && !size.isEmpty() ? size : QSize(200, 200);
    cfg.width = static_cast<uint16_t>(chosen.width());
    cfg.height = static_cast<uint16_t>(chosen.height());

    // Named on the dashboard's rule for a widget on a page, with an index that
    // only goes up -- see Canvas::addWidget for why not the page's size.
    const QString name =
        dashboard::childWidgetObjectName(objectName(), pageSlots_[page].name, cfg, nextChildIndex_++);
    SelectionFrame* frame = buildPageChild(cfg, name, editorMode);
    if (frame == nullptr)
    {
        return nullptr;
    }
    pageSlots_[page].frames.push_back(frame);
    restack();
    applyPageVisibility();
    return frame;
}

bool SelectionFrame::removePageChild(SelectionFrame* child)
{
    const auto where = locateChild(child);
    if (!where)
    {
        return false;
    }
    auto& frames = pageSlots_[where->first].frames;
    frames.erase(frames.begin() + static_cast<std::ptrdiff_t>(where->second));
    child->hide();
    child->deleteLater();
    return true;
}

std::optional<std::pair<std::size_t, std::size_t>> SelectionFrame::locateChild(const SelectionFrame* child) const
{
    for (std::size_t p = 0; p < pageSlots_.size(); ++p)
    {
        const auto& frames = pageSlots_[p].frames;
        for (std::size_t w = 0; w < frames.size(); ++w)
        {
            if (frames[w] == child)
            {
                return std::make_pair(p, w);
            }
        }
    }
    return std::nullopt;
}

SelectionFrame* SelectionFrame::containerFrame() const
{
    return qobject_cast<SelectionFrame*>(parentWidget());
}

void SelectionFrame::setScopeActive(bool on)
{
    if (scopeActive_ == on)
    {
        return;
    }
    scopeActive_ = on;
    if (overlay_)
    {
        overlay_->update();
    }
}

void SelectionFrame::restack()
{
    if (child_)
    {
        child_->lower();
    }
    for (const PageSlot& slot : pageSlots_)
    {
        for (const auto& frame : slot.frames)
        {
            if (frame)
            {
                frame->raise();
            }
        }
    }
    if (overlay_)
    {
        overlay_->raise();
    }
}

void SelectionFrame::labelPages()
{
    if (auto* stack = qobject_cast<PageStackWidget*>(child_))
    {
        std::vector<std::string> names;
        for (const PageSlot& slot : pageSlots_)
        {
            names.push_back(slot.name);
        }
        stack->setPlaceholderPageNames(std::move(names), pageSlots_.empty() ? std::nullopt
                                                                             : std::optional<std::size_t>(shownPage_));
    }
}

void SelectionFrame::ensureChild()
{
    // A page_stack from the palette arrives with no pages, and a stack with none
    // does not load. Give it the one page a new stack starts with.
    if (isContainer() && pageSlots_.empty())
    {
        applyPages(default_widget_pages(type_));
    }

    if (child_ != nullptr)
    {
        labelPages();
        return;
    }

    // No config was applied, so this frame takes the widget's own defaults --
    // and has to remember them, because config() is what gets exported. Reading
    // them back off the widget would export whatever the clamp produced instead.
    config_ = default_widget_config(type_);
    rebuildChild();
}

void SelectionFrame::rebuildChild()
{
    // Through widget_factory, not `new widget_t(cfg)`, so the preview is clamped
    // exactly as the dashboard clamps it. The editor used to skip this entirely
    // -- createWidgetFromConfig was called only by MainWindow -- so a config with
    // an out-of-range field previewed one way here and drew another way there,
    // with the editor being the optimistic one.
    //
    // config_ itself is left alone. The clamp applies to the copy the widget is
    // built from, so what gets saved is still what was configured.
    widget_config_t wc;
    wc.type = type_;
    wc.config = config_;
    setChild(widget_factory::createWidgetFromConfig(wc, nullptr));
    restack();
    labelPages();
}

void SelectionFrame::setChild(QWidget* newChild)
{
    if (child_ == newChild)
    {
        return;
    }

    if (child_)
    {
        // deleteLater alone -- do NOT setParent(nullptr) first. A parentless
        // QWidget is a top-level window, so between here and the next event-loop
        // turn the outgoing child showed up in QApplication::topLevelWidgets(),
        // which is what the agent's WidgetLocator enumerates as snapshot roots.
        // That turned an ordinary property edit into a second root and an
        // ambiguous selector.
        child_->deleteLater();
    }

    child_ = newChild;

    if (child_)
    {
        child_->setParent(this);
        child_->move(0, 0);
        // Keep frame size; resize new child to fit current frame
        child_->resize(size());
        child_->show();

        // A child added after the overlay stacks above it and hides the
        // selection chrome. paintEvent re-raises as well, but only once
        // something asks for a repaint; do it here so the frame is correct the
        // moment the child appears. This matters now that the overlay is always
        // created first -- it used to be built after the constructor's child.
        if (overlay_)
        {
            overlay_->raise();
        }
    }
}

void SelectionFrame::setSelected(bool on)
{
    if (selected_ == on)
    {
        return;
    }

    selected_ = on;
    update();
}

void SelectionFrame::setEditorModeCapture(bool on)
{
    editorMode_ = on;
    if (child_)
    {
        child_->setAttribute(Qt::WA_TransparentForMouseEvents, on);
    }
    for (const PageSlot& slot : pageSlots_)
    {
        for (const auto& frame : slot.frames)
        {
            if (frame)
            {
                frame->setEditorModeCapture(on);
            }
        }
    }

    update();
}

SelectionFrame::Handle SelectionFrame::hitTestLocal(const QPoint& pos) const
{
    const QRect r(0, 0, width(), height());
    const QRect tl(r.topLeft() - QPoint(kGrabHandleSizePx/2, kGrabHandleSizePx/2), QSize(kGrabHandleSizePx, kGrabHandleSizePx));
    const QRect tr(QPoint(r.right() - kGrabHandleSizePx/2, r.top() - kGrabHandleSizePx/2), QSize(kGrabHandleSizePx, kGrabHandleSizePx));
    const QRect bl(QPoint(r.left() - kGrabHandleSizePx/2, r.bottom() - kGrabHandleSizePx/2), QSize(kGrabHandleSizePx, kGrabHandleSizePx));
    const QRect br(QPoint(r.right() - kGrabHandleSizePx/2, r.bottom() - kGrabHandleSizePx/2), QSize(kGrabHandleSizePx, kGrabHandleSizePx));
    if (tl.contains(pos)) return Handle::ResizeTL;
    if (tr.contains(pos)) return Handle::ResizeTR;
    if (bl.contains(pos)) return Handle::ResizeBL;
    if (br.contains(pos)) return Handle::ResizeBR;
    if (r.contains(pos)) return Handle::Move;
    return Handle::None;
}

void SelectionFrame::paintEvent(QPaintEvent* /*event*/)
{
    // no selection chrome in non-editor mode
    if (!editorMode_)
    {
        return;
    }
    if (overlay_)
    {
        overlay_->raise();
    }
}

void SelectionFrame::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (child_)
    {
        child_->resize(size());
    }
    if (overlay_)
    {
        overlay_->resize(size());
    }
}

bool SelectionFrame::eventFilter(QObject* obj, QEvent* event)
{
    if (obj == overlay_ && event->type() == QEvent::Paint)
    {
        if (!editorMode_) return false;
        QPainter p(static_cast<QWidget*>(obj));
        p.setRenderHint(QPainter::Antialiasing);
        const bool drawHandles = selected_;
        const QColor outline = selected_ ? kSelectedOutlineColor
                               : scopeActive_ ? kScopeOutlineColor
                                              : kUnselectedOutlineColor;
        QPen pen(outline);
        pen.setWidth(2);
        pen.setCosmetic(true);
        if (scopeActive_ && !selected_)
        {
            pen.setStyle(Qt::DashLine);
        }
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        const QRect outer = static_cast<QWidget*>(obj)->rect().adjusted(0, 0, -1, -1);
        p.drawRect(outer);
        if (scopeActive_ && shownPage_ < pageSlots_.size())
        {
            // Which page the widgets inside belong to, in the corner of the stack
            // being edited.
            const QString tag = QString("page %1/%2: %3")
                                    .arg(shownPage_ + 1)
                                    .arg(pageSlots_.size())
                                    .arg(QString::fromStdString(pageSlots_[shownPage_].name));
            QFont font = p.font();
            font.setPointSize(10);
            p.setFont(font);
            const QRect tagRect = p.fontMetrics().boundingRect(tag).adjusted(-4, -2, 4, 2);
            const QRect placed(outer.right() - tagRect.width() - 2, outer.top() + 2, tagRect.width(), tagRect.height());
            p.fillRect(placed, kScopeOutlineColor);
            p.setPen(Qt::black);
            p.drawText(placed, Qt::AlignCenter, tag);
            p.setPen(pen);
        }
        if (drawHandles)
        {
            const QRect r = outer;
            const QRect handles[] = {
                QRect(r.topLeft() - QPoint(kGrabHandleSizePx/2, kGrabHandleSizePx/2), QSize(kGrabHandleSizePx, kGrabHandleSizePx)),
                QRect(QPoint(r.right() - kGrabHandleSizePx/2, r.top() - kGrabHandleSizePx/2), QSize(kGrabHandleSizePx, kGrabHandleSizePx)),
                QRect(QPoint(r.left() - kGrabHandleSizePx/2, r.bottom() - kGrabHandleSizePx/2), QSize(kGrabHandleSizePx, kGrabHandleSizePx)),
                QRect(QPoint(r.right() - kGrabHandleSizePx/2, r.bottom() - kGrabHandleSizePx/2), QSize(kGrabHandleSizePx, kGrabHandleSizePx))
            };
            p.setBrush(outline);
            for (const auto& h : handles) p.drawRect(h);
        }
        return true;
    }
    return QWidget::eventFilter(obj, event);
}

#include "editor/moc_selection_frame.cpp"


