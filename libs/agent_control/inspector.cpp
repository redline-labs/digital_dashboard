#include "agent_control/inspector.h"

#include <QAbstractButton>
#include <QComboBox>
#include <QLineEdit>
#include <QMetaObject>
#include <QMetaProperty>
#include <QVariant>

#include <functional>

namespace agent_control
{

namespace
{

QString classNameOf(const QWidget* widget)
{
    return QString::fromUtf8(widget->metaObject()->className());
}

// "Interactive" is a filter for token economy, not a security boundary: it keeps
// the things an agent might click or read and drops pure scaffolding. Anything
// with an explicit objectName is kept regardless -- somebody named it on purpose.
bool isInteresting(const QWidget* widget)
{
    if (!widget->objectName().isEmpty())
    {
        return true;
    }
    if (widget->isWindow())
    {
        return true;
    }
    if (widget->focusPolicy() != Qt::NoFocus)
    {
        return true;
    }

    // A custom widget (anything outside Qt's own namespace) is a first-class
    // participant here: the dashboard's gauges are all custom QWidget subclasses
    // with no focus policy, and they are exactly what the agent cares about.
    const QString name = classNameOf(widget);
    return !name.startsWith(QLatin1Char('Q'));
}

// A best-effort human-readable label. Qt has no common text() on QWidget, so ask
// the metaobject for the properties that carry one when they exist.
std::optional<QString> textOf(const QWidget* widget)
{
    static const char* kTextProperties[] = {"text", "title", "currentText", "windowTitle"};
    for (const char* property : kTextProperties)
    {
        const QVariant value = widget->property(property);
        if (value.isValid() && value.canConvert<QString>())
        {
            const QString text = value.toString();
            if (!text.isEmpty())
            {
                return text;
            }
        }
    }
    return std::nullopt;
}

json rowFor(WidgetLocator& locator, QWidget* widget)
{
    json row = json::object();
    row["ref"] = locator.refFor(widget).toStdString();
    row["path"] = WidgetLocator::pathOf(widget).toStdString();
    row["class"] = classNameOf(widget).toStdString();

    if (!widget->objectName().isEmpty())
    {
        row["id"] = widget->objectName().toStdString();
    }

    // Geometry is reported twice on purpose. `rect` is local to the widget,
    // which is the coordinate space every input method takes, so it is the one
    // an agent should compute clicks in. `window_rect` says where the widget
    // sits in its window, which is what makes an annotated full-window
    // screenshot interpretable.
    row["rect"] = json::array({0, 0, widget->width(), widget->height()});

    const QPoint origin = widget->isWindow()
                              ? QPoint(0, 0)
                              : widget->mapTo(widget->window(), QPoint(0, 0));
    row["window_rect"] =
        json::array({origin.x(), origin.y(), widget->width(), widget->height()});

    row["visible"] = widget->isVisible();
    row["enabled"] = widget->isEnabled();
    row["focus"] = widget->hasFocus();

    // Canvas::setEditorMode() applies WA_TransparentForMouseEvents recursively
    // when editor mode is off. Without this field, a click that lands on the
    // right widget and does nothing is inexplicable; with it, it is obvious.
    if (widget->testAttribute(Qt::WA_TransparentForMouseEvents))
    {
        row["mouse_transparent"] = true;
    }

    if (const auto text = textOf(widget))
    {
        row["text"] = text->toStdString();
    }

    return row;
}

}  // namespace

json describeWidget(WidgetLocator& locator, QWidget* widget)
{
    return rowFor(locator, widget);
}

json buildSnapshot(WidgetLocator& locator, const SnapshotOptions& options)
{
    std::vector<QWidget*> roots;
    if (options.root != nullptr)
    {
        roots.push_back(options.root);
    }
    else
    {
        roots = locator.roots();
    }

    std::vector<QWidget*> visited;
    json rows = json::array();

    std::function<void(QWidget*, int)> descend = [&](QWidget* widget, int depth)
    {
        visited.push_back(widget);

        const bool depth_ok = (options.max_depth < 0) || (depth <= options.max_depth);
        const bool visible_ok = options.include_invisible || widget->isVisible();
        const bool interesting_ok = !options.interactive_only || isInteresting(widget);

        if (depth_ok && visible_ok && interesting_ok)
        {
            rows.push_back(rowFor(locator, widget));
        }

        // Descend regardless of whether this widget was reported: an
        // uninteresting container can hold interesting children, and filtering
        // must not silently amputate the subtree.
        if ((options.max_depth < 0) || (depth < options.max_depth))
        {
            for (QObject* child : widget->children())
            {
                if (auto* as_widget = qobject_cast<QWidget*>(child))
                {
                    descend(as_widget, depth + 1);
                }
            }
        }
    };

    for (QWidget* root : roots)
    {
        descend(root, 0);
    }

    locator.noteTreeState(visited);

    json out = json::object();
    out["revision"] = locator.revision();
    out["count"] = rows.size();
    out["widgets"] = std::move(rows);
    return out;
}

}  // namespace agent_control
