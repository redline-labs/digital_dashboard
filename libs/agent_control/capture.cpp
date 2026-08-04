#include "agent_control/capture.h"

#include <QBuffer>
#include <QByteArray>
#include <QCryptographicHash>
#include <QImage>
#include <QMetaObject>
#include <QPixmap>

#include <functional>

namespace agent_control
{

namespace
{

// Classes whose contents are composited by the GPU or the window system rather
// than drawn into Qt's backing store. QWidget::grab() renders the backing store,
// so anything in this list captures as a black or empty rectangle.
//
// This matters here specifically: the CarPlay widget used to be a QVideoWidget
// and was reverted to a QImage blit (commit 4d143ae) for z-ordering reasons.
// That revert is the only reason screenshots of the video work at all. If it is
// ever undone, screenshots would silently start returning black -- and
// WA_NativeWindow reads FALSE on those widgets, so the obvious runtime check
// does not detect it. Hence a class-name check, and a warning in the metadata
// rather than a plausible-looking black image.
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
