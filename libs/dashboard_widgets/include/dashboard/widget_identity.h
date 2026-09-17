#ifndef DASHBOARD_WIDGET_IDENTITY_H_
#define DASHBOARD_WIDGET_IDENTITY_H_

#include "dashboard/app_config.h"
#include "reflection/reflection.h"

#include <QString>
#include <QWidget>

#include <cstddef>

namespace dashboard
{

// The single rule for naming a configured widget, shared by the dashboard's
// MainWindow and the editor's SelectionFrame so a config addressed one way in
// the editor is addressable the same way in the running dashboard.
//
// An explicit `id:` wins. Otherwise the name is derived from the widget type and
// its index in the config, which is stable for a config that nobody edits but
// shifts as soon as widgets are added, removed or reordered -- hence the
// recommendation to set `id:` on anything worth addressing repeatedly.
inline QString widgetObjectName(const widget_config_t& cfg, std::size_t index)
{
    if (!cfg.id.empty())
    {
        return QString::fromStdString(cfg.id);
    }

    return QString("%1#%2")
        .arg(QString::fromUtf8(reflection::enum_to_string(cfg.type).data(),
                               static_cast<qsizetype>(reflection::enum_to_string(cfg.type).size())))
        .arg(index);
}

// A page of a page_stack: "<stack>:<page>". Qualified by the stack so two stacks
// with a "main" page do not share a name, and with ':' so it can never collide
// with an id or a window name, neither of which may contain one.
inline QString pageObjectName(const QString& stack_name, const std::string& page_name)
{
    return stack_name + ":" + QString::fromStdString(page_name);
}

// A widget on a page. An explicit `id:` still wins -- ids are meant to be
// addressable wherever the widget lives -- otherwise the derived name is
// qualified by its page, since "<type>#<index>" repeats on every page.
inline QString childWidgetObjectName(const QString& stack_name, const std::string& page_name,
                                     const widget_config_t& cfg, std::size_t index)
{
    if (!cfg.id.empty())
    {
        return QString::fromStdString(cfg.id);
    }
    return pageObjectName(stack_name, page_name) + ":" + widgetObjectName(cfg, index);
}

// Applies the name from the rule above. Kept as a function rather than an
// inlined setObjectName() call so there is exactly one place that decides.
inline void applyWidgetIdentity(QWidget* widget, const widget_config_t& cfg, std::size_t index)
{
    if (widget != nullptr)
    {
        widget->setObjectName(widgetObjectName(cfg, index));
    }
}

}  // namespace dashboard

#endif  // DASHBOARD_WIDGET_IDENTITY_H_
