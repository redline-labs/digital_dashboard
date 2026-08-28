#include "agent_control/capture.h"

#include <QBuffer>
#include <QByteArray>
#include <QCryptographicHash>
#include <QFont>
#include <QImage>
#include <QMetaObject>
#include <QPainter>
#include <QPixmap>

#include <cmath>
#include <functional>

namespace agent_control
{

namespace
{

// Classes whose contents may not reach the image QWidget::grab() produces.
//
// The reason differs by class, and the distinction is worth stating because the
// obvious one is WRONG for most of this list:
//
//   * QVideoWidget is a NATIVE widget -- the window system composites it, Qt
//     never sees the pixels, and grab() genuinely returns black. This is the
//     case that matters here: the CarPlay widget used to be one and was
//     reverted to a QImage blit (commit 4d143ae) for z-ordering. That revert is
//     the only reason video screenshots work.
//
//   * QOpenGLWidget, QQuickWidget and QRhiWidget are RENDER-TO-TEXTURE widgets,
//     which Qt composites itself. MEASURED on Qt 6.11/cocoa: a QRhiWidget's
//     content DOES come back through a parent's grab(), and a child widget over
//     it composites correctly on top. All three also expose grabFramebuffer().
//     So on a real display they are not a capture problem at all.
//
//     They are still listed because of how THIS process runs. Under
//     QT_QPA_PLATFORM=offscreen -- which is every agent-control session and
//     every gui test -- the platform reports no RHI support, so such a widget
//     never initialises and draws nothing. Empty, rather than black, but just
//     as useless in a screenshot. (Verified on macOS; the Linux offscreen
//     plugin can be built with GL support, where this may not hold.)
//
// WA_NativeWindow reads FALSE on all of them, so the obvious runtime check does
// not detect any of this. Hence a class-name check, and a warning in the
// metadata rather than a plausible-looking empty image.
bool isNonBackingStoreClass(const QString& class_name)
{
    return class_name == QLatin1String("QVideoWidget") ||
           class_name == QLatin1String("QOpenGLWidget") ||
           class_name == QLatin1String("QRhiWidget") ||
           class_name == QLatin1String("QQuickWidget");
}

std::vector<QString> findUncapturableClasses(const QWidget* widget)
{
    std::vector<QString> found;

    std::function<void(const QWidget*)> descend = [&](const QWidget* node)
    {
        const QString name = QString::fromUtf8(node->metaObject()->className());
        if (isNonBackingStoreClass(name))
        {
            found.push_back(name);
        }
        for (const QObject* child : node->children())
        {
            if (const auto* as_widget = qobject_cast<const QWidget*>(child))
            {
                descend(as_widget);
            }
        }
    };

    descend(widget);
    return found;
}

// Children worth marking: visible, big enough to see, and identifiable. Qt's own
// scaffolding is skipped -- a box round a layout container tells the reader
// nothing and crowds out the widgets that matter.
std::vector<QWidget*> annotationTargets(const QWidget* root)
{
    std::vector<QWidget*> targets;

    std::function<void(const QWidget*)> descend = [&](const QWidget* node)
    {
        for (QObject* child : node->children())
        {
            auto* as_widget = qobject_cast<QWidget*>(child);
            if (as_widget == nullptr)
            {
                continue;
            }

            const QString class_name = QString::fromUtf8(as_widget->metaObject()->className());
            const bool identifiable =
                !as_widget->objectName().isEmpty() || !class_name.startsWith(QLatin1Char('Q'));
            const bool big_enough = as_widget->width() >= 16 && as_widget->height() >= 16;

            if (as_widget->isVisible() && identifiable && big_enough)
            {
                targets.push_back(as_widget);
                // Do not descend into a marked widget: the interesting target is
                // the gauge, not the labels inside it.
                continue;
            }
            descend(as_widget);
        }
    };

    descend(root);
    return targets;
}

// Draws the marks onto an image that is still in *logical* widget coordinates,
// before any downscale. Doing it after would need the marks scaled too, and the
// line widths would stop being legible at small sizes.
json drawAnnotations(QImage& image, QWidget* root, const QRect& region, qreal dpr)
{
    const auto targets = annotationTargets(root);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, false);

    QFont font = painter.font();
    font.setPixelSize(11);
    font.setBold(true);
    painter.setFont(font);

    json marks = json::array();
    int index = 1;

    for (QWidget* target : targets)
    {
        const QPoint origin = target->mapTo(root, QPoint(0, 0)) - region.topLeft();
        QRect box(origin, target->size());
        if (!box.intersects(QRect(QPoint(0, 0), region.size())))
        {
            continue;
        }

        // The image is in device pixels; the geometry is logical.
        const QRect device_box(QPoint(static_cast<int>(box.x() * dpr),
                                      static_cast<int>(box.y() * dpr)),
                               QSize(static_cast<int>(box.width() * dpr),
                                     static_cast<int>(box.height() * dpr)));

        painter.setPen(QPen(QColor(255, 64, 0), 2));
        painter.drawRect(device_box.adjusted(0, 0, -1, -1));

        const QString label = QString::number(index);
        const QRect label_box(device_box.topLeft(), QSize(18, 16));
        painter.fillRect(label_box, QColor(255, 64, 0));
        painter.setPen(Qt::white);
        painter.drawText(label_box, Qt::AlignCenter, label);

        json mark = json::object();
        mark["mark"] = index;
        mark["selector"] = !target->objectName().isEmpty()
                               ? "#" + target->objectName().toStdString()
                               : WidgetLocator::pathOf(target).toStdString();
        mark["class"] = target->metaObject()->className();
        // Local to the CAPTURED widget, which is the space input.click takes for
        // that same target -- so a mark can be turned into a click directly.
        mark["rect"] = json::array({box.x(), box.y(), box.width(), box.height()});
        marks.push_back(std::move(mark));

        ++index;
    }

    return marks;
}

}  // namespace

Result<json> captureWidget(WidgetLocator& locator, QWidget* widget, const CaptureOptions& options)
{
    if (widget == nullptr)
    {
        return std::unexpected(internalError("captureWidget received a null widget."));
    }

    if (widget->width() <= 0 || widget->height() <= 0)
    {
        json data = json::object();
        data["target"] = WidgetLocator::pathOf(widget).toStdString();
        data["size"] = json::array({widget->width(), widget->height()});
        return std::unexpected(AgentError{
            ErrorCode::kWidgetNotVisible,
            "Widget has no area to capture; it has not been laid out or shown.",
            std::move(data)});
    }

    QRect region = options.region.isNull() ? widget->rect() : options.region;
    region = region.intersected(widget->rect());
    if (region.isEmpty())
    {
        json data = json::object();
        data["target"] = WidgetLocator::pathOf(widget).toStdString();
        data["widget_rect"] =
            json::array({0, 0, widget->width(), widget->height()});
        return std::unexpected(
            badParams("Requested region does not intersect the widget."));
    }

    const QPixmap pixmap = widget->grab(region);
    if (pixmap.isNull())
    {
        return std::unexpected(internalError("QWidget::grab() returned a null pixmap."));
    }

    QImage image = pixmap.toImage();
    const qreal dpr = pixmap.devicePixelRatio();

    json marks;
    if (options.annotate)
    {
        marks = drawAnnotations(image, widget, region, dpr);
    }

    // Scale from the *logical* size, so `scale` relates the returned image to
    // widget-local coordinates directly and the caller never has to reason about
    // device pixel ratio.
    const int longest = std::max(region.width(), region.height());
    double scale = 1.0;
    if (options.max_dim > 0 && longest > options.max_dim)
    {
        scale = static_cast<double>(options.max_dim) / static_cast<double>(longest);
        const int target_w = std::max(1, static_cast<int>(std::lround(region.width() * scale)));
        const int target_h = std::max(1, static_cast<int>(std::lround(region.height() * scale)));
        image = image.scaled(target_w, target_h, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    else if (dpr != 1.0)
    {
        // No downscale requested, but the backing store is oversampled. Report
        // the true ratio rather than pretending it is 1.
        scale = static_cast<double>(image.width()) / static_cast<double>(region.width());
    }

    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, "PNG"))
    {
        return std::unexpected(internalError("Failed to encode the capture as PNG."));
    }
    buffer.close();

    const QByteArray digest = QCryptographicHash::hash(png, QCryptographicHash::Sha256);
    const std::string hash = digest.toHex().left(16).toStdString();

    json out = json::object();
    out["target"] = WidgetLocator::pathOf(widget).toStdString();
    out["ref"] = locator.refFor(widget).toStdString();
    out["logical_rect"] =
        json::array({region.x(), region.y(), region.width(), region.height()});
    out["image_size"] = json::array({image.width(), image.height()});
    out["scale"] = scale;
    out["dpr"] = static_cast<double>(dpr);
    out["revision"] = locator.revision();
    out["hash"] = hash;

    if (options.annotate)
    {
        out["marks"] = std::move(marks);
    }

    const auto uncapturable = findUncapturableClasses(widget);
    if (!uncapturable.empty())
    {
        json classes = json::array();
        for (const QString& name : uncapturable)
        {
            classes.push_back(name.toStdString());
        }
        json warning = json::object();
        warning["reason"] = "non_backing_store_widget";
        warning["classes"] = std::move(classes);
        warning["detail"] =
            "This subtree contains widgets whose contents are composited outside "
            "Qt's backing store, so they capture as black. The image is not a "
            "faithful view of what is on screen.";
        out["warning"] = std::move(warning);
    }

    if (!options.if_changed_from.empty() && options.if_changed_from == hash)
    {
        out["unchanged"] = true;
        return out;
    }

    out["unchanged"] = false;
    out["image_png_base64"] = png.toBase64().toStdString();
    return out;
}

}  // namespace agent_control
