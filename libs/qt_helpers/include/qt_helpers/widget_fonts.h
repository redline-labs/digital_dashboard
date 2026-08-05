#ifndef QT_HELPERS_WIDGET_FONTS_H_
#define QT_HELPERS_WIDGET_FONTS_H_

#include <QFont>
#include <QFontDatabase>
#include <QHash>
#include <QString>

#include <spdlog/spdlog.h>

namespace qt_helpers {

// Loads a font from Qt resources and returns its family name. Falls back to
// `fallback_family` (or the application default when empty) with a warning if
// the resource can't be loaded.
//
// Memoized, per resource path. QFontDatabase::addApplicationFont() reads the
// resource and parses the TrueType tables every time it is called, and returns
// a fresh id each time -- so repeated calls are both slow and a slow leak of
// registered application fonts.
//
// That mattered in two ways. Seven widgets call this from their constructors,
// all for the same futura.ttf, so a dashboard re-parsed one font seven times at
// startup. Worse, now_playing called it from inside paintEvent, re-registering
// the font on every single repaint.
inline QString loadResourceFont(const char* resource_path, const QString& fallback_family = QString())
{
    // Function-local static: this is only ever touched from the GUI thread, and
    // the alternative is an init-order dependency on a namespace-scope QHash.
    static QHash<QString, QString> cache;

    const QString key = QString::fromUtf8(resource_path);
    if (const auto it = cache.constFind(key); it != cache.constEnd())
    {
        return it.value();
    }

    const int font_id = QFontDatabase::addApplicationFont(resource_path);
    if (font_id == -1)
    {
        SPDLOG_WARN("Failed to load font '{}'. Using fallback.", resource_path);
        const QString fallback = fallback_family.isEmpty() ? QFont().family() : fallback_family;

        // Cached too: a missing resource stays missing, and without this the
        // warning would repeat for the lifetime of the process.
        cache.insert(key, fallback);
        return fallback;
    }

    const QStringList families = QFontDatabase::applicationFontFamilies(font_id);
    if (families.isEmpty())
    {
        // Qt accepted the file but found no families in it. `.at(0)` here used
        // to be an out-of-range assertion.
        SPDLOG_WARN("Font '{}' loaded but declares no families. Using fallback.", resource_path);
        const QString fallback = fallback_family.isEmpty() ? QFont().family() : fallback_family;
        cache.insert(key, fallback);
        return fallback;
    }

    cache.insert(key, families.at(0));
    return families.at(0);
}

}  // namespace qt_helpers

#endif  // QT_HELPERS_WIDGET_FONTS_H_
