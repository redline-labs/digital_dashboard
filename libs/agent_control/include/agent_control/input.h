#ifndef AGENT_CONTROL_INPUT_H_
#define AGENT_CONTROL_INPUT_H_

#include "agent_control/error.h"

#include <QByteArray>
#include <QPointF>
#include <QString>
#include <QWidget>

#include <string>
#include <utility>
#include <vector>

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

struct DragOptions
{
    QPointF from;
    QPointF to;
    Qt::MouseButton button = Qt::LeftButton;
    Qt::KeyboardModifiers modifiers = Qt::NoModifier;
    int steps = 10;     // Intermediate move events between press and release.
    int hold_ms = 0;    // Pause after the press, before moving.
};

// Press, N interpolated moves, release -- all widget-local.
//
// Covers the two drags in this project that are plain mouse tracking: a CarPlay
// swipe (which becomes TOUCH_DOWN/MOVE/UP, coalesced by TouchThrottle at ~60 Hz)
// and the editor Canvas's own move/resize handling.
//
// It does NOT drive Qt's QDrag: QDrag::exec() runs a nested event loop that
// grabs the mouse and reads real platform events, so synthesized events cannot
// advance it. The palette-to-canvas drag needs sendDrop() instead.
Result<json> sendDrag(QWidget* widget, const DragOptions& options);

// Synthesizes the QDragEnter -> QDragMove -> QDrop triple directly on a drop
// target, with a QMimeData built from `mime` (format -> UTF-8 payload).
//
// This deliberately bypasses the drag *source*: QDrag::exec() cannot be driven
// by synthesized events, and on the offscreen platform may not run at all. What
// it does exercise is the whole of the receiving side -- dragEnterEvent's
// accept/reject logic and dropEvent's handling -- which is where the behaviour
// worth testing actually lives.
Result<json> sendDrop(QWidget* target,
                      const QPointF& pos,
                      const std::vector<std::pair<QString, QByteArray>>& mime,
                      Qt::DropAction action);

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
