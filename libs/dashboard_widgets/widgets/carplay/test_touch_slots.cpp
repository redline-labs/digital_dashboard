// SPDX-License-Identifier: GPL-3.0-or-later
// Which finger gets which of the phone's two contact slots, and what the frame
// says when fingers land, move and lift. No Qt, no zenoh, no clock.
#include "carplay/touch_slots.h"

#include <spdlog/spdlog.h>

#include <string>

namespace
{

int failures = 0;

void expect(bool condition, const std::string& what)
{
    if (!condition)
    {
        SPDLOG_ERROR("FAIL: {}", what);
        ++failures;
    }
}

const TouchSlots::Contact& slot(const TouchSlots& s, size_t i)
{
    return s.frame().contacts[i];
}

void testFirstFingerTakesSlotZero()
{
    TouchSlots s;
    expect(s.press(7, 10, 20), "a finger on empty glass is taken on");
    expect(slot(s, 0).active && slot(s, 0).down && slot(s, 0).x == 10 && slot(s, 0).y == 20,
           "it lands in slot 0 where it touched");
    expect(!slot(s, 1).active, "slot 1 stays empty");
}

void testSecondFingerTakesTheOtherSlot()
{
    TouchSlots s;
    s.press(7, 10, 20);
    expect(s.press(3, 30, 40), "a second finger is taken on");
    expect(slot(s, 1).active && slot(s, 1).x == 30, "it lands in slot 1");
    expect(s.move(3, 31, 41) && slot(s, 1).x == 31 && slot(s, 0).x == 10,
           "moving one finger moves only its own slot");
}

void testThirdFingerIsNotReported()
{
    // Evicting a finger still on the glass would tell the phone it lifted.
    TouchSlots s;
    s.press(1, 0, 0);
    s.press(2, 0, 0);
    expect(!s.press(3, 50, 50), "a third finger is refused");
    expect(!s.move(3, 51, 51), "and its motion is ignored");
    expect(!s.release(3, 51, 51), "and so is its lift");
}

void testLiftIsReportedThenFreesTheSlot()
{
    TouchSlots s;
    s.press(1, 0, 0);
    s.press(2, 5, 5);
    expect(s.release(1, 9, 9), "a finger that holds a slot can lift");
    expect(slot(s, 0).active && !slot(s, 0).down && slot(s, 0).x == 9,
           "the frame reporting the lift still carries it, up, where it lifted");
    s.commit();
    expect(!slot(s, 0).active, "once published, the lifted finger leaves its slot");
    expect(slot(s, 1).active && slot(s, 1).down, "and the other finger keeps its own");
}

void testSlotIsStableAcrossTheOtherFingerLifting()
{
    // The phone tracks a finger by its slot: the survivor must not shift to 0.
    TouchSlots s;
    s.press(1, 0, 0);
    s.press(2, 5, 5);
    s.release(1, 0, 0);
    s.commit();
    s.move(2, 6, 6);
    expect(slot(s, 1).x == 6 && !slot(s, 0).active, "the remaining finger stays in slot 1");

    expect(s.press(4, 8, 8) && slot(s, 0).x == 8,
           "a new finger takes the freed lower slot");
}

void testReleaseAllLiftsEveryFinger()
{
    TouchSlots s;
    expect(!s.releaseAll(), "nothing to lift on empty glass");
    s.press(1, 1, 1);
    s.press(2, 2, 2);
    expect(s.releaseAll(), "fingers down are lifted");
    expect(!slot(s, 0).down && !slot(s, 1).down && slot(s, 0).active && slot(s, 1).active,
           "both are reported up in the same frame");
    s.commit();
    expect(!slot(s, 0).active && !slot(s, 1).active, "and both slots are then free");
    expect(!s.move(1, 3, 3), "the rest of a cancelled sequence names ids that hold nothing");
}

void testSameIdCannotLandTwice()
{
    TouchSlots s;
    s.press(1, 1, 1);
    expect(!s.press(1, 2, 2), "an id already down is not taken on again");
    expect(!slot(s, 1).active, "and does not occupy a second slot");
}

void testFramesCompareByContent()
{
    // The throttle relies on == to skip a duplicate before a lift.
    TouchSlots a;
    TouchSlots b;
    a.press(1, 1, 1);
    b.press(9, 1, 1);
    expect(a.frame() == b.frame(), "the same fingers in the same places are the same frame");
    b.move(9, 2, 1);
    expect(!(a.frame() == b.frame()), "a moved finger is a different frame");
}

}  // namespace

int main()
{
    testFirstFingerTakesSlotZero();
    testSecondFingerTakesTheOtherSlot();
    testThirdFingerIsNotReported();
    testLiftIsReportedThenFreesTheSlot();
    testSlotIsStableAcrossTheOtherFingerLifting();
    testReleaseAllLiftsEveryFinger();
    testSameIdCannotLandTwice();
    testFramesCompareByContent();

    if (failures == 0)
    {
        SPDLOG_INFO("all touch slot tests passed");
    }
    return failures == 0 ? 0 : 1;
}
