#ifndef DASHBOARD_WIDGET_COLORS_H_
#define DASHBOARD_WIDGET_COLORS_H_

#include <QColor>
#include <QString>

#include <spdlog/spdlog.h>

#include "helpers/color.h"

namespace dashboard {

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
    const QColor parsed = QColor::fromString(QString::fromStdString(color.value()));
    if (parsed.isValid())
    {
        return parsed;
    }

    SPDLOG_WARN("'{}' is not a colour Qt understands; falling back to {}.",
                color.value(), fallback.name().toStdString());
    return fallback;
}

}  // namespace dashboard

#endif  // DASHBOARD_WIDGET_COLORS_H_
