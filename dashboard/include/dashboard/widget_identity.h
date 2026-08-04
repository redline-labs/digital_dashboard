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
