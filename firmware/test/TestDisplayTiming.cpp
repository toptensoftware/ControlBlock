#include "UnitTesting.h"
#include "Core.h"
#include "display_timing.h"

using namespace SimpleLib;

// Display timing at any pixel clock (the frame rate experiment, wip/wip-frame-rate-experiment.md).
// The panel's numbers: 15-line bounce fills, 820 clocks a line, 500 lines a frame.

static const uint32_t LINES = 15, H_TOTAL = 820, FRAME_CLOCKS = 820 * 500;

Fact("DisplayTiming Fill Spacing Matches The Compile Time Budget At 20 MHz")
{
    // DISPLAY_BOUNCE_BUDGET_US = 15 * 820 / 20 = 615 us
    Assert(DisplayFillSpacingUs(LINES, H_TOTAL, 20000000) == 615);
    Assert(DisplayFillSpacingCycles(LINES, H_TOTAL, 20000000, 240) == 615 * 240);
}

Fact("DisplayTiming Fill Spacing Grows As The Clock Drops")
{
    Assert(DisplayFillSpacingUs(LINES, H_TOTAL, 16000000) == 768);     // 768.75
    Assert(DisplayFillSpacingUs(LINES, H_TOTAL, 12000000) == 1025);
    Assert(DisplayFillSpacingUs(LINES, H_TOTAL, 10000000) == 1230);
    Assert(DisplayFillSpacingUs(LINES, H_TOTAL, 6000000) == 2050);
    // Cycles keep the fraction the us figure drops
    Assert(DisplayFillSpacingCycles(LINES, H_TOTAL, 16000000, 240) == 184500);
    Assert(DisplayFillSpacingCycles(LINES, H_TOTAL, 6000000, 240) == 492000);
}

Fact("DisplayTiming Fps From Frames Over Time")
{
    Assert(DisplayFpsX10(49, 1000000) == 490);
    // 20 MHz: a frame is 20.5 ms, 48.78 fps
    Assert(DisplayFpsX10(390, 390ull * 20500) == 488);
    Assert(DisplayFpsX10(0, 1000000) == 0);
    Assert(DisplayFpsX10(10, 0) == 0);
}

Fact("DisplayTiming Pixel Clock From Measured Frame Periods")
{
    Assert(DisplayPclkFromPeriods(FRAME_CLOCKS, 1, 20500) == 20000000);
    Assert(DisplayPclkFromPeriods(FRAME_CLOCKS, 3, 102500) == 12000000);    // 34166.7 us a frame
    // Rounded to the nearest Hz: 6 MHz frames are 68333.3 us
    uint32_t hz = DisplayPclkFromPeriods(FRAME_CLOCKS, 1, 68333);
    Assert(hz > 5999990 && hz < 6000100);
    Assert(DisplayPclkFromPeriods(FRAME_CLOCKS, 1, 0) == 0);
    Assert(DisplayPclkFromPeriods(FRAME_CLOCKS, 0, 20500) == 0);
}

Fact("DisplayTiming Period Matches A Clock Within A Tolerance")
{
    Assert(DisplayPeriodMatches(FRAME_CLOCKS, 20500, 20000000, 20));
    Assert(DisplayPeriodMatches(FRAME_CLOCKS, 20800, 20000000, 20));        // 1.5% off
    Assert(!DisplayPeriodMatches(FRAME_CLOCKS, 21000, 20000000, 20));       // 2.4% off
    Assert(!DisplayPeriodMatches(FRAME_CLOCKS, 51250, 20000000, 20));       // an 8 MHz frame
    Assert(DisplayPeriodMatches(FRAME_CLOCKS, 51250, 8000000, 20));
    Assert(!DisplayPeriodMatches(FRAME_CLOCKS, 20500, 0, 20));
}

Fact("DisplayTiming Schedule Steps Down And Back Up Then Toggles")
{
    Assert(PCLK_SCHEDULE_LEN > 7);
    Assert(PclkScheduleStep(0).mhz == 20);
    // Every step is a sane clock, never above the 20 MHz the fill gaps assume (fill_gap.h), and held
    bool seen[21] = {};
    int toggles = 0;
    for (int i = 0; i < PCLK_SCHEDULE_LEN; i++) {
        PclkStep s = PclkScheduleStep(i);
        Assert(s.mhz >= 1 && s.mhz <= 20);
        Assert(s.hold_s >= 1);
        seen[s.mhz] = true;
        if (s.hold_s == 1)
            toggles++;
    }
    Assert(seen[16] && seen[12] && seen[10] && seen[8] && seen[6]);
    Assert(toggles >= 4);
    // The schedule repeats, and the last step changes the clock back to the first's
    Assert(PclkScheduleStep(PCLK_SCHEDULE_LEN).mhz == PclkScheduleStep(0).mhz);
    Assert(PclkScheduleStep(PCLK_SCHEDULE_LEN + 3).mhz == PclkScheduleStep(3).mhz);
    Assert(PclkScheduleStep(PCLK_SCHEDULE_LEN - 1).mhz != PclkScheduleStep(0).mhz);
    // Each step changes the clock, so every hold starts with a switch
    for (int i = 0; i < PCLK_SCHEDULE_LEN; i++)
        Assert(PclkScheduleStep(i).mhz != PclkScheduleStep(i + 1).mhz);
}

// Phase 2: a longer vertical blanking (front porch) at a fixed clock

Fact("DisplayTiming Front Porch Is What The Total Leaves")
{
    // VSYNC 4 + back porch 8 + 480 active: the build's 500 lines leave a front porch of 8
    Assert(DisplayVfpForTotal(500, 4, 8, 480) == 8);
    Assert(DisplayVfpForTotal(1024, 4, 8, 480) == 532);
    Assert(DisplayVfpForTotal(492, 4, 8, 480) == 0);
    Assert(DisplayVfpForTotal(400, 4, 8, 480) == 0);       // too short: no porch at all
    // The LCD_CAM's vertical total is 10 bits (lcd_vt_height = lines - 1)
    Assert(DISPLAY_V_TOTAL_HW_MAX == 1024);
}

Fact("DisplayTiming Fps From Clock And Total Lines")
{
    Assert(DisplayTimingFpsX10(20000000, H_TOTAL, 500) == 488);     // 48.78
    Assert(DisplayTimingFpsX10(20000000, H_TOTAL, 1000) == 244);    // 24.39
    Assert(DisplayTimingFpsX10(20000000, H_TOTAL, 1024) == 238);    // 23.82
    Assert(DisplayTimingFpsX10(16000000, H_TOTAL, 500) == 390);     // 39.02
    Assert(DisplayTimingFpsX10(16000000, H_TOTAL, 1024) == 191);    // 19.05
    Assert(DisplayTimingFpsX10(20000000, H_TOTAL, 0) == 0);
}

Fact("DisplayTiming Total Lines For A Target Fps")
{
    Assert(DisplayVTotalForFpsX10(20000000, H_TOTAL, 488, 500) == 500);     // 499.8
    Assert(DisplayVTotalForFpsX10(20000000, H_TOTAL, 300, 500) == 813);     // 813.0
    Assert(DisplayVTotalForFpsX10(16000000, H_TOTAL, 250, 500) == 780);     // 780.5 -> 780
    // Clamped: never shorter than the build's frame, never past the hardware's 1024
    Assert(DisplayVTotalForFpsX10(20000000, H_TOTAL, 600, 500) == 500);
    Assert(DisplayVTotalForFpsX10(20000000, H_TOTAL, 100, 500) == 1024);
    Assert(DisplayVTotalForFpsX10(20000000, H_TOTAL, 0, 500) == 1024);
}

Fact("DisplayTiming Total Lines From Measured Frame Periods")
{
    // 1000 lines at 20 MHz: 41 ms a frame
    Assert(DisplayVTotalFromPeriods(H_TOTAL, 20000000, 1, 41000) == 1000);
    Assert(DisplayVTotalFromPeriods(H_TOTAL, 20000000, 4, 4 * 20500) == 500);
    Assert(DisplayVTotalFromPeriods(H_TOTAL, 16000000, 2, 2 * 52480) == 1024);
    Assert(DisplayVTotalFromPeriods(H_TOTAL, 20000000, 1, 41010) == 1000);  // rounded
    Assert(DisplayVTotalFromPeriods(H_TOTAL, 20000000, 0, 41000) == 0);
    Assert(DisplayVTotalFromPeriods(H_TOTAL, 0, 1, 41000) == 0);
}

Fact("DisplayTiming Pixel Clock Schedule Keeps The Build's Lines")
{
    for (int i = 0; i < PCLK_SCHEDULE_LEN; i++)
        Assert(PclkScheduleStep(i).lines == DISPLAY_TIMING_V_TOTAL);
}

Fact("DisplayTiming Vblank Schedule Changes One Thing A Step")
{
    Assert(VBLANK_SCHEDULE_LEN > 10);
    Assert(VblankScheduleStep(0).mhz == 20 && VblankScheduleStep(0).lines == DISPLAY_TIMING_V_TOTAL);
    bool lines_at[2][5] = {};       // [20 MHz, 16 MHz][600, 750, 900, 1024, 1000]
    const uint16_t wanted[5] = { 600, 750, 900, 1024, 1000 };
    int toggles = 0;
    for (int i = 0; i < VBLANK_SCHEDULE_LEN; i++) {
        PclkStep s = VblankScheduleStep(i);
        PclkStep n = VblankScheduleStep(i + 1);
        // Only the panel's proven clocks (16-20 MHz), never above the fill gaps' 20 MHz
        Assert(s.mhz == 16 || s.mhz == 20);
        // Within what the hardware holds, never shorter than the build's frame
        Assert(s.lines >= DISPLAY_TIMING_V_TOTAL && s.lines <= DISPLAY_V_TOTAL_HW_MAX);
        Assert(s.hold_s >= 1);
        // Exactly one of the clock and the lines changes at each step (the wrap included)
        Assert((s.mhz != n.mhz) != (s.lines != n.lines));
        for (int k = 0; k < 5; k++)
            if (s.lines == wanted[k])
                lines_at[s.mhz == 16][k] = true;
        if (s.hold_s == 1) {
            toggles++;
            Assert(s.mhz == 20 && (s.lines == DISPLAY_TIMING_V_TOTAL || s.lines == 1000));
        }
    }
    for (int k = 0; k < 4; k++)
        Assert(lines_at[0][k] && lines_at[1][k]);
    Assert(lines_at[0][4]);
    Assert(toggles >= 8);
    Assert(VblankScheduleStep(VBLANK_SCHEDULE_LEN + 2).lines == VblankScheduleStep(2).lines);
}

// Phase 3: the timing as a setting (DISPLAY_SET)

Fact("DisplayTiming Limits For The Build's Clock")
{
    DisplayTimingLimits l = DisplayTimingLimitsFor(20);
    Assert(l.default_mhz == 20 && l.min_mhz == DISPLAY_PCLK_MIN_MHZ && l.max_mhz == 20);
    Assert(DISPLAY_PCLK_MIN_MHZ == 16);                                     // the panel's floor (phase 1)
    Assert(l.default_lines == DISPLAY_TIMING_V_TOTAL && l.min_lines == DISPLAY_TIMING_V_TOTAL);
    Assert(l.max_lines == DISPLAY_V_TOTAL_HW_MAX);

    // Never above the build's clock (the fill gaps' deadline assumes it); a build below the
    // panel's floor allows only its own clock
    DisplayTimingLimits slow = DisplayTimingLimitsFor(12);
    Assert(slow.min_mhz == 12 && slow.max_mhz == 12 && slow.default_mhz == 12);
    DisplayTimingLimits fast = DisplayTimingLimitsFor(25);
    Assert(fast.min_mhz == 16 && fast.max_mhz == 25);
}

Fact("DisplayTiming Valid Within The Limits Only")
{
    DisplayTimingLimits l = DisplayTimingLimitsFor(20);
    Assert(DisplayTimingValid(l, 20, 500));
    Assert(DisplayTimingValid(l, 16, 1024));
    Assert(DisplayTimingValid(l, 18, 750));
    Assert(!DisplayTimingValid(l, 15, 500));
    Assert(!DisplayTimingValid(l, 21, 500));
    Assert(!DisplayTimingValid(l, 20, 499));
    Assert(!DisplayTimingValid(l, 20, 1025));
    Assert(!DisplayTimingValid(l, 0, 500));
}

Fact("DisplayTiming Hold Stats Keep Maxima And Sum Counts")
{
    FillHoldStats h;
    Assert(h.seconds == 0 && h.fill_max_us == 0 && h.missed == 0);
    h.Add(100, 300, 0, 0, 0, 0);
    h.Add(250, 200, 2, 1, 1, 0);
    h.Add(90, 700, 1, 0, 0, 3);
    Assert(h.seconds == 3);
    Assert(h.fill_max_us == 250);
    Assert(h.end_max_us == 700);
    Assert(h.missed == 3);
    Assert(h.overruns == 1);
    Assert(h.seq_breaks == 1);
    Assert(h.buffer_mismatches == 3);
    h.Reset();
    Assert(h.seconds == 0 && h.end_max_us == 0 && h.buffer_mismatches == 0);
}
