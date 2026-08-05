#include "editor/selection_frame.h"
#include "editor/widget_registry.h"

#include "dashboard/widget_factory.h"

#include <QPainter>
#include <QEvent>

namespace
{
    // Colors for selection chrome
    // kSelectedOutlineColor: Active selection outline color (medium blue)
    // kUnselectedOutlineColor: Non-selected frame outline color in editor mode (dark gray)
    constexpr QColor kSelectedOutlineColor(0, 122, 255);
    constexpr QColor kUnselectedOutlineColor(80, 80, 80);
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

void SelectionFrame::ensureChild()
{
    if (child_ != nullptr)
    {
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

    update();
}

SelectionFrame::Handle SelectionFrame::hitTestCanvasPos(const QPoint& canvasPos) const
{
    const QPoint pos = mapFromParent(canvasPos);
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
        const QColor outline = selected_ ? kSelectedOutlineColor : kUnselectedOutlineColor;
        QPen pen(outline);
        pen.setWidth(2);
        pen.setCosmetic(true);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        const QRect outer = static_cast<QWidget*>(obj)->rect().adjusted(0, 0, -1, -1);
        p.drawRect(outer);
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


