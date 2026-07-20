#ifndef DASHBOARD_WIDGET_FONTS_H_
#define DASHBOARD_WIDGET_FONTS_H_

#include <QFont>
#include <QFontDatabase>
#include <QString>

#include <spdlog/spdlog.h>

namespace dashboard {

// Loads a font from Qt resources and returns its family name. Falls back to
// `fallback_family` (or the application default when empty) with a warning if
// the resource can't be loaded.
inline QString loadResourceFont(const char* resource_path, const QString& fallback_family = QString())
{
    const int font_id = QFontDatabase::addApplicationFont(resource_path);
    if (font_id == -1)
    {
        SPDLOG_WARN("Failed to load font '{}'. Using fallback.", resource_path);
        return fallback_family.isEmpty() ? QFont().family() : fallback_family;
    }
    return QFontDatabase::applicationFontFamilies(font_id).at(0);
}

}  // namespace dashboard

#endif  // DASHBOARD_WIDGET_FONTS_H_
