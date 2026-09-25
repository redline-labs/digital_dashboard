// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef CARPLAY_TOUCH_SLOTS_H_
#define CARPLAY_TOUCH_SLOTS_H_

#include <array>
#include <cstddef>

// Which finger is in which of the phone's contact slots, and the touch frame
// the widget publishes from that.
//
// The phone's digitizer has two slots (airplay::hid::kTouchContacts; not
// included from there because the widget does not link the driver's library)
// and tracks a finger by the slot it keeps appearing in. So a finger keeps the
// slot it landed in until it lifts, a new finger takes the lowest free one, and
// a third finger is not reported at all -- the alternative, evicting a finger
// that is still down, would read to the phone as a lift nobody made.
//
// Policy and state only, like TouchThrottle: no Qt, no clock, no publisher.
// Fingers are named by an id the caller keeps stable for the finger's time on
// the glass -- a QEventPoint id for touch, a fixed one for the mouse.
class TouchSlots
{
  public:
    static constexpr int kSlots = 2;

    struct Contact
    {
        bool active = false;  // a finger occupies this slot in this frame
        bool down = false;    // false for a finger this frame lifts
        double x = 0.0;
        double y = 0.0;

        friend bool operator==(const Contact&, const Contact&) = default;
    };

    // Every slot at one instant. Published whole, as one message.
    struct Frame
    {
        std::array<Contact, kSlots> contacts{};

        friend bool operator==(const Frame&, const Frame&) = default;
    };

    // A finger landed. False when it was not taken on: every slot is occupied,
    // or the id is already down.
    bool press(int id, double x, double y)
    {
        if (find(id) >= 0)
        {
            return false;
        }
        for (size_t i = 0; i < _frame.contacts.size(); ++i)
        {
            if (!_frame.contacts[i].active)
            {
                _frame.contacts[i] = {true, true, x, y};
                _ids[i] = id;
                return true;
            }
        }
        return false;
    }

    // Motion. False for a finger that holds no slot -- a third finger, or a
    // mouse moving with no button down.
    bool move(int id, double x, double y)
    {
        const int slot = find(id);
        if (slot < 0)
        {
            return false;
        }
        auto& c = _frame.contacts[static_cast<size_t>(slot)];
        c.x = x;
        c.y = y;
        return true;
    }

    // A finger lifted at (x, y). It stays in the frame, marked up, until
    // commit(): the frame that reports the lift has to carry it.
    bool release(int id, double x, double y)
    {
        const int slot = find(id);
        if (slot < 0)
        {
            return false;
        }
        auto& c = _frame.contacts[static_cast<size_t>(slot)];
        c.down = false;
        c.x = x;
        c.y = y;
        return true;
    }

    // Every finger lifts where it is. For a cancelled touch sequence and for a
    // widget going off screen: the phone must not be left holding a finger.
    // False when there was nothing to lift.
    bool releaseAll()
    {
        bool any = false;
        for (size_t i = 0; i < _frame.contacts.size(); ++i)
        {
            if (_frame.contacts[i].active && _frame.contacts[i].down)
            {
                _frame.contacts[i].down = false;
                any = true;
            }
        }
        return any;
    }

    const Frame& frame() const { return _frame; }

    // The frame has been published: fingers it lifted give up their slots.
    void commit()
    {
        for (size_t i = 0; i < _frame.contacts.size(); ++i)
        {
            if (_frame.contacts[i].active && !_frame.contacts[i].down)
            {
                _frame.contacts[i] = {};
                _ids[i] = kNoId;
            }
        }
    }

  private:
    // No real finger has this id: QEventPoint ids are non-negative, and the
    // widget's mouse id is chosen away from it.
    static constexpr int kNoId = -1000;

    int find(int id) const
    {
        for (size_t i = 0; i < _ids.size(); ++i)
        {
            if (_frame.contacts[i].active && _ids[i] == id)
            {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    Frame _frame;
    std::array<int, kSlots> _ids{kNoId, kNoId};
};

#endif  // CARPLAY_TOUCH_SLOTS_H_
