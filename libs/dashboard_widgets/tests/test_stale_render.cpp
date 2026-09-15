// SPDX-License-Identifier: GPL-3.0-or-later
//
// Two rules about loss of comm, over every widget in the table.
//
// 1. A widget bound to a topic implements StaleAware. Otherwise a new gauge
//    holds its last reading forever, which is the failure this whole mechanism
//    exists to retire, and nothing would say so.
// 2. Going stale CHANGES WHAT IS DRAWN. A widget can implement the interface,
//    set its flag, repaint, and look identical -- and then the driver sees a
//    steady reading from a dead sensor exactly as before. Rendering both states
//    and requiring the images to differ is what makes the look real.
#include "dashboard/stale_aware.h"
#include "dashboard/widget_registry.h"
#include "dashboard/widget_table.h"

#include "reflection/reflection.h"

#include <QApplication>
#include <QImage>
#include <QPainter>

#include <cstdio>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace
{

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const std::string& what)
{
    ++g_checks;
    if (!condition)
    {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

// Widgets whose streams are events rather than readings: CarPlay video and its
// metadata, now playing, and the road-name horizon. A silence there is a phone
// with nothing to say, not a fault, and the liveness that matters for them is
// the carplay node's own health.
bool isEventDriven(std::string_view widget)
{
    return widget == "carplay" || widget == "carplay_nav" || widget == "now_playing" ||
           widget == "road_info";
}

// Bound through a nested config struct, which the top-level field names below
// cannot see. Listed so the first rule still covers them.
bool isNestedBinding(std::string_view widget)
{
    return widget == "mercedes_190e_cluster_gauge";
}

// The map's stale look needs a position to draw at, which only arrives through
// its subscriptions. map_test_widget covers it with one.
bool rendersOnlyWithData(std::string_view widget)
{
    return widget == "map";
}

template <typename Cfg>
bool bindsATopic()
{
    if constexpr (reflection::field_metadata_traits<Cfg>::has_metadata)
    {
        for (const auto& field : reflection::field_metadata_traits<Cfg>::metadata())
        {
            if (field.field_name.find("zenoh_key") != std::string_view::npos)
            {
                return true;
            }
        }
    }
    return false;
}

QImage renderOf(QWidget& widget)
{
    QImage canvas(widget.size(), QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::black);
    widget.render(&canvas);
    return canvas;
}

template <typename Widget>
void checkOne(std::string_view name)
{
    constexpr bool aware = std::is_base_of_v<dashboard::StaleAware, Widget>;

    if ((bindsATopic<typename Widget::config_t>() || isNestedBinding(name)) && !isEventDriven(name))
    {
        check(aware, std::string(name) + " reads a topic, so it must implement StaleAware");
    }

    if constexpr (aware)
    {
        if (rendersOnlyWithData(name))
        {
            return;
        }

        typename Widget::config_t config;
        Widget widget(config);
        widget.resize(240, 160);

        auto& stale_aware = static_cast<dashboard::StaleAware&>(widget);
        const QImage fresh = renderOf(widget);

        for (const std::string_view binding : stale_aware.staleBindings())
        {
            stale_aware.setBindingStale(binding, true);
            const QImage stale = renderOf(widget);
            const std::string what = std::string(name) + "'s " + std::string(binding) + " binding";

            check(stale != fresh, what + " draws differently when it goes stale");
            check(widget.property("stale").toBool(), what + " reports itself stale");
            check(stale_aware.isBindingStale(binding), what + " remembers it is stale");

            stale_aware.setBindingStale(binding, false);
            check(!stale_aware.anyBindingStale(), what + " clears again");
            check(renderOf(widget) == fresh, what + " draws as it did before once data returns");
        }
    }
}

}  // namespace

int main(int argc, char** argv)
{
    // No display, and no bus: a widget's default config binds no topic, so
    // nothing here subscribes.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

#define DASHBOARD_WIDGET_STALE_CHECK(enum_name, widget_class) checkOne<widget_class>(#enum_name);
    DASHBOARD_WIDGET_TABLE(DASHBOARD_WIDGET_STALE_CHECK)
#undef DASHBOARD_WIDGET_STALE_CHECK

    std::fprintf(stderr, "%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
