#ifndef DASHBOARD_CACHED_PAINT_WIDGET_H_
#define DASHBOARD_CACHED_PAINT_WIDGET_H_

#include <QPainter>
#include <QPixmap>
#include <QWidget>

namespace dashboard {

// QWidget base that composes a repaint as: cached static underlay pixmap →
// per-frame dynamic content → optional cached static overlay pixmap. The
// static layers are rendered through the subclass hooks only when the widget
// size changes (or invalidateStaticCache() is called), so per-frame work is
// limited to paintDynamic(). All hooks receive a painter with
// applyPaintTransform() already applied.
//
// No Q_OBJECT: subclasses keep their own meta-object via QWidget.
class CachedPaintWidget : public QWidget {
  public:
    using QWidget::QWidget;

  protected:
    // Shared logical transform (e.g. center + scale to a logical canvas).
    virtual void applyPaintTransform(QPainter&) const {}

    // Static content drawn below the dynamic layer; cached in a pixmap.
    virtual void paintStaticUnderlay(QPainter&) {}

    // Content redrawn every frame (needles, value arcs, ...).
    virtual void paintDynamic(QPainter&) = 0;

    // Static content drawn above the dynamic layer; cached in a pixmap.
    // Subclasses that use it must also override hasStaticOverlay().
    virtual void paintStaticOverlay(QPainter&) {}
    virtual bool hasStaticOverlay() const { return false; }

    // Force the static layers to re-render on the next repaint (e.g. after a
    // state change that alters static content).
    void invalidateStaticCache() { cached_size_ = QSize(); }

    void paintEvent(QPaintEvent*) final
    {
        if (width() <= 0 || height() <= 0)
        {
            return;
        }

        if (cached_size_ != size())
        {
            underlay_ = renderLayer(&CachedPaintWidget::paintStaticUnderlay);
            overlay_ = hasStaticOverlay() ? renderLayer(&CachedPaintWidget::paintStaticOverlay) : QPixmap();
            cached_size_ = size();
        }

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.drawPixmap(0, 0, underlay_);

        painter.save();
        applyPaintTransform(painter);
        paintDynamic(painter);
        painter.restore();

        if (!overlay_.isNull())
        {
            painter.drawPixmap(0, 0, overlay_);
        }
    }

  private:
    QPixmap renderLayer(void (CachedPaintWidget::*hook)(QPainter&))
    {
        // Render at the device pixel ratio so cached layers stay sharp on
        // high-DPI displays.
        const qreal dpr = devicePixelRatioF();
        QPixmap layer(size() * dpr);
        layer.setDevicePixelRatio(dpr);
        layer.fill(Qt::transparent);
        QPainter painter(&layer);
        painter.setRenderHint(QPainter::Antialiasing);
        applyPaintTransform(painter);
        (this->*hook)(painter);
        return layer;
    }

    QPixmap underlay_;
    QPixmap overlay_;
    QSize cached_size_;
};

}  // namespace dashboard

#endif  // DASHBOARD_CACHED_PAINT_WIDGET_H_
