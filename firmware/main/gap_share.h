#pragma once
#include "port.h"

// Fill gaps (the ~400 us after each bounce fill) are the only time PSRAM may
// be written. Several tasks want them (the compositor copying key slices, the
// status bar copying rows), so each gap goes to one client, in turn, and
// two writers never overlap. Pure; fill_gap.cpp uses it on the device.

class GapRotor
{
public:
    // The client to give this gap to: the next one after the last that
    // `wants`, or -1 if none does.
    IRAM_ATTR int Next(const bool *wants, int count)
    {
        for (int i = 1; i <= count; i++) {
            int c = (m_last + i) % count;
            if (wants[c]) {
                m_last = c;
                return c;
            }
        }
        return -1;
    }

private:
    int m_last = -1;
};
