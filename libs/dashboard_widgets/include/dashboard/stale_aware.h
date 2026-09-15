// SPDX-License-Identifier: GPL-3.0-or-later
//
// What a widget implements so that its no-data state is visible from outside:
// to a test that renders it, and to the agent-control snapshot.
#ifndef DASHBOARD_STALE_AWARE_H_
#define DASHBOARD_STALE_AWARE_H_

#include <QVariant>
#include <QWidget>

#include <string>
#include <string_view>
#include <vector>

namespace dashboard
{

// A widget with at least one bound reading.
//
// The names are the widget's own: "value", "speed", "odometer", "position".
// One per binding, because bindings go stale independently -- a speedometer
// whose odometer stream has stopped still shows the speed.
class StaleAware
{
  public:
    virtual ~StaleAware() = default;

    virtual std::vector<std::string_view> staleBindings() const = 0;
    virtual void setBindingStale(std::string_view binding, bool stale) = 0;
    virtual bool isBindingStale(std::string_view binding) const = 0;

    bool anyBindingStale() const
    {
        for (const std::string_view binding : staleBindings())
        {
            if (isBindingStale(binding))
            {
                return true;
            }
        }
        return false;
    }
};

// Mirrors the state onto Qt properties, so `ui_snapshot` can report it without
// agent_control knowing what a widget is. Call it from setBindingStale().
inline void publishStaleProperties(QWidget& widget, const StaleAware& aware)
{
    std::string names;
    for (const std::string_view binding : aware.staleBindings())
    {
        if (!aware.isBindingStale(binding))
        {
            continue;
        }
        if (!names.empty())
        {
            names += ",";
        }
        names += std::string(binding);
    }
    widget.setProperty("stale", !names.empty());
    widget.setProperty("stale_bindings", QString::fromStdString(names));
}

}  // namespace dashboard

#endif  // DASHBOARD_STALE_AWARE_H_
