#ifndef AGENT_CONTROL_INPUT_H_
#define AGENT_CONTROL_INPUT_H_

#include "agent_control/error.h"

#include <QPointF>
#include <QString>
#include <QWidget>

#include <string>

namespace agent_control
{

// All positions are WIDGET-LOCAL LOGICAL PIXELS. This is the single coordinate
// rule of the whole interface, and it is what makes the CarPlay widget need no
// special handling: it normalises pos.x()/width() itself before publishing
// touches, so a local coordinate derived from a screenshot of that widget is
// already the right thing to send.
struct ClickOptions
{
    QPointF pos;               // Widget-local. Defaults to the widget centre.
    bool pos_specified = false;
    Qt::MouseButton button = Qt::LeftButton;
    Qt::KeyboardModifiers modifiers = Qt::NoModifier;
    int count = 1;             // 2 => double click.
};

// Sends a press/release pair (and the DoubleClick event when count == 2) to
// `widget` via QApplication::sendEvent, mirroring what
// dashboard/widgets/carplay/test_touch_rate.cpp already does.
Result<json> sendClick(QWidget* widget, const ClickOptions& options);

// A key sequence in QKeySequence's text form: "Ctrl+S", "Delete", "F5".
// Delivered to `widget`, or to the focus widget when `widget` is null.
Result<json> sendKeySequence(QWidget* widget, const QString& sequence);

// Literal text, one QKeyEvent pair per character.
Result<json> sendText(QWidget* widget, const QString& text);

// Parses "left"/"right"/"middle" and a "+"-joined modifier list.
Result<Qt::MouseButton> parseMouseButton(const std::string& name);
Result<Qt::KeyboardModifiers> parseModifiers(const std::string& spec);

}  // namespace agent_control

#endif  // AGENT_CONTROL_INPUT_H_
