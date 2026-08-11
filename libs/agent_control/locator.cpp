#include "agent_control/locator.h"

#include <QApplication>
#include <QMetaObject>
#include <QRegularExpression>

#include <algorithm>
#include <functional>

namespace agent_control
{

namespace
{

// Widgets Qt creates behind our back and that no agent ever wants to address:
// the scroll-area viewport, the internal container of a scroll view, and so on.
// They are still traversed (their children matter), just not offered as
// candidates in error messages.
bool isNoiseWidget(const QWidget* widget)
{
    const QString name = QString::fromUtf8(widget->metaObject()->className());
    return name == QLatin1String("QWidget") && widget->objectName().startsWith(QLatin1String("qt_"));
}

QString classNameOf(const QWidget* widget)
{
    return QString::fromUtf8(widget->metaObject()->className());
}

// Index of `widget` among its siblings sharing its class. Returns -1 when it is
// the only one, which is the signal to omit the [n] suffix entirely.
int siblingIndexOf(const QWidget* widget)
{
    const QObject* parent = widget->parent();
    if (parent == nullptr)
    {
        return -1;
    }

    const QString wanted = classNameOf(widget);
    int index = 0;
    int found = -1;
    int total = 0;
    for (const QObject* sibling : parent->children())
    {
        const auto* as_widget = qobject_cast<const QWidget*>(sibling);
        if (as_widget == nullptr || classNameOf(as_widget) != wanted)
        {
            continue;
        }
        if (as_widget == widget)
        {
            found = index;
        }
        ++index;
        ++total;
    }

    return (total > 1) ? found : -1;
}

}  // namespace

void WidgetLocator::setRoots(std::vector<QWidget*> roots)
{
    roots_.clear();
    roots_.reserve(roots.size());
    for (QWidget* w : roots)
    {
        roots_.emplace_back(w);
    }
}

std::vector<QWidget*> WidgetLocator::roots() const
{
    std::vector<QWidget*> out;

    if (!roots_.empty())
    {
        for (const auto& p : roots_)
        {
            if (!p.isNull())
            {
                out.push_back(p.data());
            }
        }
        return out;
    }

    // Default: every top-level window. Qt counts a lot of transient things as
    // top-level -- an editor with a menu bar reports each QMenu as its own
    // top-level widget, which would otherwise show up as a sibling of the main
    // window in every snapshot. Keep only real windows.
    const auto top_level = QApplication::topLevelWidgets();
    for (QWidget* w : top_level)
    {
        if (w == nullptr || !w->isWindow())
        {
            continue;
        }

        // An if-chain rather than a switch: Qt::WindowType has 33 enumerators and
        // the build runs with -Wswitch-enum, so a switch here would mean listing
        // every one of them to reject a handful.
        //
        // Qt::Desktop is deliberately absent. QDesktopWidget was removed in Qt 6
        // and the flag has been a no-op ever since, so no widget can report it --
        // testing for it rejects nothing and, from Qt 6.10 on, is a
        // -Wdeprecated-declarations error under -Werror.
        const Qt::WindowType type = w->windowType();
        if (type == Qt::Popup || type == Qt::ToolTip || type == Qt::SplashScreen ||
            type == Qt::SubWindow)
        {
            continue;
        }

        out.push_back(w);
    }
    return out;
}

std::vector<QWidget*> WidgetLocator::allWidgets() const
{
    std::vector<QWidget*> out;

    std::function<void(QWidget*)> descend = [&](QWidget* widget)
    {
        out.push_back(widget);
        for (QObject* child : widget->children())
        {
            if (auto* as_widget = qobject_cast<QWidget*>(child))
            {
                descend(as_widget);
            }
        }
    };

    for (QWidget* root : roots())
    {
        descend(root);
    }
    return out;
}

QString WidgetLocator::pathOf(const QWidget* widget)
{
    if (widget == nullptr)
    {
        return {};
    }

    QStringList segments;
    for (const QWidget* node = widget; node != nullptr;)
    {
        QString segment = classNameOf(node);
        const int index = siblingIndexOf(node);
        if (index >= 0)
        {
            segment += QString("[%1]").arg(index);
        }
        segments.prepend(segment);

        if (node->isWindow())
        {
            break;
        }
        node = qobject_cast<const QWidget*>(node->parent());
    }

    return segments.join(QLatin1Char('/'));
}

QString WidgetLocator::refFor(QWidget* widget)
{
    if (widget == nullptr)
    {
        return {};
    }

    for (std::size_t i = 0; i < refs_.size(); ++i)
    {
        if (refs_[i].data() == widget)
        {
            return QString("w%1").arg(i);
        }
    }

    refs_.emplace_back(widget);
    return QString("w%1").arg(refs_.size() - 1);
}

Result<QWidget*> WidgetLocator::widgetForRef(const std::string& ref) const
{
    // Caller has already matched the w<digits> shape.
    const std::size_t index = std::stoull(ref.substr(1));
    if (index >= refs_.size())
    {
        json data = json::object();
        data["ref"] = ref;
        data["revision"] = revision_;
        return std::unexpected(AgentError{ErrorCode::kStaleRef,
                                          "Ref '" + ref + "' was never issued. Take a ui.snapshot.",
                                          std::move(data)});
    }

    // QPointer nulls itself when the widget is destroyed, which is precisely the
    // case worth reporting distinctly: the agent's mental model is out of date
    // rather than its selector being wrong.
    if (refs_[index].isNull())
    {
        json data = json::object();
        data["ref"] = ref;
        data["revision"] = revision_;
        return std::unexpected(
            AgentError{ErrorCode::kStaleRef,
                       "Ref '" + ref + "' points at a widget that has been destroyed. "
                                       "Take a ui.snapshot.",
                       std::move(data)});
    }

    return refs_[index].data();
}

std::vector<QWidget*> WidgetLocator::matchAnywhere(const QString& token) const
{
    std::vector<QWidget*> matches;
    for (QWidget* widget : allWidgets())
    {
        if (widget->objectName() == token || classNameOf(widget) == token)
        {
            matches.push_back(widget);
        }
    }
    return matches;
}

json WidgetLocator::describeCandidates(const std::vector<QWidget*>& widgets) const
{
    json out = json::array();
    for (QWidget* widget : widgets)
    {
        json entry = json::object();
        entry["path"] = pathOf(widget).toStdString();
        entry["class"] = classNameOf(widget).toStdString();
        if (!widget->objectName().isEmpty())
        {
            entry["id"] = widget->objectName().toStdString();
        }
        out.push_back(std::move(entry));
    }
    return out;
}

Result<QWidget*> WidgetLocator::resolvePath(const std::string& selector)
{
    const QString path = QString::fromStdString(selector);
    const QStringList segments = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (segments.isEmpty())
    {
        return std::unexpected(badParams("Empty selector."));
    }

    static const QRegularExpression kIndexed(QStringLiteral("^(.*)\\[(\\d+)\\]$"));

    // Candidate set starts as the roots and narrows one segment at a time.
    std::vector<QWidget*> current = roots();
    bool first = true;

    for (const QString& raw_segment : segments)
    {
        QString token = raw_segment;
        int wanted_index = -1;
        const auto match = kIndexed.match(raw_segment);
        if (match.hasMatch())
        {
            token = match.captured(1);
            wanted_index = match.captured(2).toInt();
        }

        std::vector<QWidget*> next;
        if (first)
        {
            for (QWidget* root : current)
            {
                if (root->objectName() == token || classNameOf(root) == token)
                {
                    next.push_back(root);
                }
            }
        }
        else
        {
            for (QWidget* parent : current)
            {
                for (QObject* child : parent->children())
                {
                    auto* as_widget = qobject_cast<QWidget*>(child);
                    if (as_widget == nullptr)
                    {
                        continue;
                    }
                    if (as_widget->objectName() == token || classNameOf(as_widget) == token)
                    {
                        next.push_back(as_widget);
                    }
                }
            }
        }

        if (wanted_index >= 0)
        {
            if (wanted_index >= static_cast<int>(next.size()))
            {
                return std::unexpected(noSuchWidget(
                    selector, describeCandidates(next)));
            }
            QWidget* picked = next[static_cast<std::size_t>(wanted_index)];
            next.clear();
            next.push_back(picked);
        }

        current = std::move(next);
        first = false;

        if (current.empty())
        {
            return std::unexpected(noSuchWidget(selector, json::array()));
        }
    }

    if (current.size() > 1)
    {
        return std::unexpected(ambiguousSelector(selector, describeCandidates(current)));
    }
    return current.front();
}

Result<QWidget*> WidgetLocator::resolve(const std::string& selector)
{
    if (selector.empty())
    {
        return std::unexpected(badParams("Selector must not be empty."));
    }

    // w<digits> -- a ref.
    if (selector[0] == 'w' && selector.size() > 1 &&
        std::all_of(selector.begin() + 1, selector.end(),
                    [](unsigned char c) { return std::isdigit(c) != 0; }))
    {
        return widgetForRef(selector);
    }

    // #name -- objectName, explicitly.
    if (selector[0] == '#')
    {
        const QString wanted = QString::fromStdString(selector.substr(1));
        std::vector<QWidget*> matches;
        for (QWidget* widget : allWidgets())
        {
            if (widget->objectName() == wanted)
            {
                matches.push_back(widget);
            }
        }
        if (matches.empty())
        {
            // Offer the named widgets that do exist: the usual cause is a typo
            // or an id that was never set in the YAML.
            std::vector<QWidget*> named;
            for (QWidget* widget : allWidgets())
            {
                if (!widget->objectName().isEmpty() && !isNoiseWidget(widget))
                {
                    named.push_back(widget);
                }
            }
            if (named.size() > 8u)
            {
                named.resize(8u);
            }
            return std::unexpected(noSuchWidget(selector, describeCandidates(named)));
        }
        if (matches.size() > 1)
        {
            return std::unexpected(ambiguousSelector(selector, describeCandidates(matches)));
        }
        return matches.front();
    }

    if (selector.find('/') != std::string::npos)
    {
        return resolvePath(selector);
    }

    // Bare token: objectName or class, anywhere. This is where the derived
    // "<type>#<index>" names land too, since they are just objectNames.
    const QString token = QString::fromStdString(selector);
    std::vector<QWidget*> matches = matchAnywhere(token);
    if (matches.empty())
    {
        return std::unexpected(noSuchWidget(selector, json::array()));
    }
    if (matches.size() > 1)
    {
        return std::unexpected(ambiguousSelector(selector, describeCandidates(matches)));
    }
    return matches.front();
}

void WidgetLocator::noteTreeState(const std::vector<QWidget*>& widgets)
{
    // Cheap structural signature: which widgets exist, in what order, with what
    // names. It intentionally ignores geometry and visibility -- those change
    // constantly and do not invalidate anything the agent knows.
    std::size_t signature = widgets.size();
    for (const QWidget* widget : widgets)
    {
        const std::size_t h = std::hash<const void*>{}(static_cast<const void*>(widget)) ^
                              std::hash<std::string>{}(widget->objectName().toStdString());
        signature = signature * 1099511628211ull ^ h;
    }

    if (signature != signature_)
    {
        signature_ = signature;
        ++revision_;
    }
}

}  // namespace agent_control
