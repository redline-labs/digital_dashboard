#ifndef TOUCH_ACTION_H_
#define TOUCH_ACTION_H_

#include <string_view>

enum class TouchAction
{
    Down = 14,
    Move = 15,
    Up = 16,
};


constexpr std::string_view touch_action_to_string(TouchAction action)
{
    switch (action)
    {
        case (TouchAction::Down):
            return "Down";

        case (TouchAction::Move):
            return "Move";

        case (TouchAction::Up):
            return "Up";

        default:
            return "Invalid";
    }
}

#endif  // TOUCH_ACTION_H_