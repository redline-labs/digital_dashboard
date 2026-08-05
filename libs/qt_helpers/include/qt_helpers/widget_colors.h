#ifndef QT_HELPERS_WIDGET_COLORS_H_
#define QT_HELPERS_WIDGET_COLORS_H_

#include <QColor>
#include <QString>

#include <spdlog/spdlog.h>

#include <string>

#include "helpers/color.h"

namespace qt_helpers {

// The one way to turn a configured colour into a QColor.
//
// There were five spellings of this across the widgets -- QColor(QString::
// fromStdString(...)), QColor::fromString(...), a local toQColor(), and a
// stylesheet built by string concatenation -- and not one of them checked
// whether the result was valid. An invalid QColor paints as transparent black,
// and an invalid value in a stylesheet makes Qt drop the entire rule, so a
// mistyped colour produced an invisible widget with nothing said about it.
//
// Config loading rejects malformed colours before they get here, so reaching the
// fallback means either a colour that arrived some other way -- the agent
// interface, a live edit in the editor -- or a form the validator allows and
// Qt does not. Either is worth a line in the log.
inline QColor toQColor(const helpers::Color& color, const QColor& fallback = Qt::black)
{
    const std::string& text = color.value();

    // "#" + 8 hex digits has to be split by hand, because the two sides disagree
    // about what it means. helpers::Color::isValidFormat documents and accepts
    // it as #RRGGBBAA; Qt reads it as #AARRGGBB. Handing it straight to Qt
    // therefore rotated every channel: "#112233ff" -- opaque dark blue -- came
    // back as r=0x22 g=0x33 b=0xff with alpha 0x11, a different colour and all
    // but invisible. Nothing complained, because the result is a perfectly valid
    // QColor. Verified against Qt 6.10 before this was written.
    //
    // Only this form is special-cased. Everything else keeps going through
    // QColor, which understands more spellings than isValidFormat does (named
    // colours, #RRRGGGBBB) and is the more permissive of the two on purpose.
    if (text.size() == 9 && text.front() == '#')
    {
        QColor parsed = QColor::fromString(QString::fromStdString(text.substr(0, 7)));
        bool alpha_ok = false;
        const int alpha = QString::fromStdString(text.substr(7, 2)).toInt(&alpha_ok, 16);
        if (parsed.isValid() && alpha_ok)
        {
            parsed.setAlpha(alpha);
            return parsed;
        }
    }

    const QColor parsed = QColor::fromString(QString::fromStdString(text));
    if (parsed.isValid())
    {
        return parsed;
    }

    SPDLOG_WARN("'{}' is not a colour Qt understands; falling back to {}.",
                color.value(), fallback.name().toStdString());
    return fallback;
}

}  // namespace qt_helpers

#endif  // QT_HELPERS_WIDGET_COLORS_H_
