#include "agent_control/input.h"

#include "agent_control/locator.h"

#include <QApplication>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMouseEvent>

#include <algorithm>
#include <cctype>

namespace agent_control
{

namespace
{

// Qt6 QMouseEvent wants three positions. Everything the agent addresses is
// widget-local, and the widgets in this project read position() (the CarPlay
// widget does exactly that), so local and scene positions are the same value.
// The global position is filled in from mapToGlobal for the benefit of anything
// that asks, but nothing in this codebase depends on it.
QMouseEvent makeMouseEvent(QEvent::Type type,
                           QWidget* widget,
                           const QPointF& local,
                           Qt::MouseButton button,
                           Qt::MouseButtons buttons,
                           Qt::KeyboardModifiers modifiers)
{
    const QPointF global = widget->mapToGlobal(local);
    return QMouseEvent(type, local, local, global, button, buttons, modifiers);
}

bool positionIsInside(const QWidget* widget, const QPointF& pos)
{
    return pos.x() >= 0.0 && pos.y() >= 0.0 && pos.x() < widget->width() &&
           pos.y() < widget->height();
}

json actionReport(QWidget* widget, const QPointF& pos)
{
    json out = json::object();
    out["target"] = WidgetLocator::pathOf(widget).toStdString();
    out["pos"] = json::array({pos.x(), pos.y()});
    return out;
}

}  // namespace

Result<Qt::MouseButton> parseMouseButton(const std::string& name)
{
    if (name.empty() || name == "left")
    {
        return Qt::LeftButton;
    }
    if (name == "right")
    {
        return Qt::RightButton;
    }
    if (name == "middle")
    {
        return Qt::MiddleButton;
    }
    return std::unexpected(
        badParams("Unknown mouse button '" + name + "'; expected left, right or middle."));
}

Result<Qt::KeyboardModifiers> parseModifiers(const std::string& spec)
{
    Qt::KeyboardModifiers modifiers = Qt::NoModifier;
    if (spec.empty())
    {
        return modifiers;
    }

    std::string token;
    auto apply = [&modifiers](const std::string& t) -> bool
    {
        if (t == "ctrl" || t == "control")
        {
            modifiers |= Qt::ControlModifier;
        }
        else if (t == "shift")
        {
            modifiers |= Qt::ShiftModifier;
        }
        else if (t == "alt" || t == "option")
        {
            modifiers |= Qt::AltModifier;
        }
        else if (t == "meta" || t == "cmd" || t == "command")
        {
            modifiers |= Qt::MetaModifier;
        }
        else if (!t.empty())
        {
            return false;
        }
        return true;
    };

    for (const char c : spec)
    {
        if (c == '+')
        {
            if (!apply(token))
            {
                return std::unexpected(badParams("Unknown modifier '" + token + "'."));
            }
            token.clear();
        }
        else
        {
            token.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    if (!apply(token))
    {
        return std::unexpected(badParams("Unknown modifier '" + token + "'."));
    }

    return modifiers;
}

Result<json> sendClick(QWidget* widget, const ClickOptions& options)
{
    if (widget == nullptr)
    {
        return std::unexpected(internalError("sendClick received a null widget."));
    }

    if (!widget->isVisible())
    {
        json data = json::object();
        data["target"] = WidgetLocator::pathOf(widget).toStdString();
        return std::unexpected(AgentError{ErrorCode::kWidgetNotVisible,
                                          "Widget is not visible, so a click cannot reach it.",
                                          std::move(data)});
    }

    // Reported rather than refused. Qt would deliver the event anyway, and a
    // caller sometimes wants exactly that; what it must not do is silently
    // believe the click was handled.
    const bool transparent = widget->testAttribute(Qt::WA_TransparentForMouseEvents);

    QPointF pos = options.pos;
    if (!options.pos_specified)
    {
        pos = QPointF(widget->width() / 2.0, widget->height() / 2.0);
    }

    if (!positionIsInside(widget, pos))
    {
        json data = json::object();
        data["target"] = WidgetLocator::pathOf(widget).toStdString();
        data["pos"] = json::array({pos.x(), pos.y()});
        data["widget_rect"] = json::array({0, 0, widget->width(), widget->height()});
        return std::unexpected(badParams(
            "Position is outside the widget. Coordinates are widget-local logical "
            "pixels; divide screenshot pixels by the reported 'scale' first."));
    }

    QMouseEvent press = makeMouseEvent(QEvent::MouseButtonPress, widget, pos, options.button,
                                       options.button, options.modifiers);
    QApplication::sendEvent(widget, &press);

    if (options.count >= 2)
    {
        QMouseEvent second = makeMouseEvent(QEvent::MouseButtonDblClick, widget, pos,
                                            options.button, options.button, options.modifiers);
        QApplication::sendEvent(widget, &second);
    }

    QMouseEvent release = makeMouseEvent(QEvent::MouseButtonRelease, widget, pos, options.button,
                                         Qt::NoButton, options.modifiers);
    QApplication::sendEvent(widget, &release);

    json out = actionReport(widget, pos);
    out["accepted"] = press.isAccepted();
    if (transparent)
    {
        out["warning"] =
            "Widget has WA_TransparentForMouseEvents set, so it does not normally "
            "receive mouse input. The event was delivered directly anyway.";
    }
    return out;
}

Result<json> sendKeySequence(QWidget* widget, const QString& sequence)
{
    QWidget* target = widget != nullptr ? widget : QApplication::focusWidget();
    if (target == nullptr)
    {
        return std::unexpected(
            badParams("No target given and nothing currently has focus."));
    }

    const QKeySequence parsed = QKeySequence::fromString(sequence, QKeySequence::PortableText);
    if (parsed.isEmpty())
    {
        return std::unexpected(badParams(
            "Could not parse key sequence '" + sequence.toStdString() +
            "'. Use portable text such as 'Ctrl+S', 'Delete' or 'F5'."));
    }

    json applied = json::array();
    for (int i = 0; i < parsed.count(); ++i)
    {
        const QKeyCombination combo = parsed[i];
        const int key = combo.key();
        const Qt::KeyboardModifiers modifiers = combo.keyboardModifiers();

        // QKeySequence gives no text for the combination, and widgets that read
        // event->text() (line edits) need it. Derive it for the printable range
        // only; everything else is a named key where text() is legitimately empty.
        QString text;
        if (key >= Qt::Key_Space && key <= Qt::Key_AsciiTilde &&
            (modifiers & ~Qt::ShiftModifier) == Qt::NoModifier)
        {
            const auto ch = static_cast<char16_t>(key);
            text = QString(QChar(ch));
            if ((modifiers & Qt::ShiftModifier) == 0)
            {
                text = text.toLower();
            }
        }

        QKeyEvent press(QEvent::KeyPress, key, modifiers, text);
        QApplication::sendEvent(target, &press);
        QKeyEvent release(QEvent::KeyRelease, key, modifiers, text);
        QApplication::sendEvent(target, &release);

        applied.push_back(QKeySequence(combo).toString(QKeySequence::PortableText).toStdString());
    }

    json out = json::object();
    out["target"] = WidgetLocator::pathOf(target).toStdString();
    out["keys"] = std::move(applied);
    return out;
}

Result<json> sendText(QWidget* widget, const QString& text)
{
    QWidget* target = widget != nullptr ? widget : QApplication::focusWidget();
    if (target == nullptr)
    {
        return std::unexpected(
            badParams("No target given and nothing currently has focus."));
    }

    for (const QChar ch : text)
    {
        const QString one(ch);
        // Key_unknown is correct here: we are delivering text, not a physical
        // key, and widgets that insert text read event->text() rather than key().
        QKeyEvent press(QEvent::KeyPress, Qt::Key_unknown, Qt::NoModifier, one);
        QApplication::sendEvent(target, &press);
        QKeyEvent release(QEvent::KeyRelease, Qt::Key_unknown, Qt::NoModifier, one);
        QApplication::sendEvent(target, &release);
    }

    json out = json::object();
    out["target"] = WidgetLocator::pathOf(target).toStdString();
    out["text"] = text.toStdString();
    out["length"] = text.size();
    return out;
}

}  // namespace agent_control
