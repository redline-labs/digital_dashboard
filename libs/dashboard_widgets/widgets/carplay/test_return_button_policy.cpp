// SPDX-License-Identifier: GPL-3.0-or-later
//
// When the CarPlay widget offers a way off its page: the whole truth table.
#include "carplay/return_button_policy.h"

#include <cstdio>

int main()
{
    int failures = 0;
    for (int bits = 0; bits < 16; ++bits)
    {
        const bool enabled = (bits & 1) != 0;
        const bool fresh = (bits & 2) != 0;
        const bool connected = (bits & 4) != 0;
        const bool recording = (bits & 8) != 0;

        // Hidden only when disabled, or when a phone is actually on screen.
        const bool live = fresh && connected && recording;
        const bool expected = enabled && !live;
        if (carplay::returnButtonVisible(enabled, fresh, connected, recording) != expected)
        {
            ++failures;
            std::fprintf(stderr, "FAIL: enabled=%d fresh=%d connected=%d recording=%d should be %s\n", enabled,
                         fresh, connected, recording, expected ? "visible" : "hidden");
        }
    }

    // The cases worth naming, so a failure reads as a sentence.
    if (!carplay::returnButtonVisible(true, false, true, true))
    {
        ++failures;
        std::fprintf(stderr, "FAIL: a stale 'recording' session must still show the button\n");
    }
    if (carplay::returnButtonVisible(true, true, true, true))
    {
        ++failures;
        std::fprintf(stderr, "FAIL: a live session must hide the button\n");
    }

    std::fprintf(stderr, "%d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
