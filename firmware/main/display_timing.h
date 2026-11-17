#pragma once
#include <stdint.h>

// Display timing at any pixel clock and frame size: the stored setting's limits (DISPLAY_SET,
// display_config.h) and the frame rate experiment's maths and schedules (PCLK_CYCLE,
// wip/wip-frame-rate-experiment.md). The compile-time figures in waveshare_rgb_lcd_port.h
// (DISPLAY_BOUNCE_BUDGET_US) only hold at the build's clock. Pure (host tested,
// TestDisplayTiming.cpp); task context only (64-bit divisions).

// The time from one bounce fill's start to the next's: `lines` lines of `h_total` clocks
constexpr uint32_t DisplayFillSpacingUs(uint32_t lines, uint32_t h_total, uint32_t pclk_hz)
{
    return pclk_hz ? (uint32_t)((uint64_t)lines * h_total * 1000000u / pclk_hz) : 0;
}

// The same in CPU cycles (keeps the fraction the us figure drops)
constexpr uint32_t DisplayFillSpacingCycles(uint32_t lines, uint32_t h_total, uint32_t pclk_hz, uint32_t cpu_mhz)
{
    return pclk_hz ? (uint32_t)((uint64_t)lines * h_total * cpu_mhz * 1000000u / pclk_hz) : 0;
}

// Frames per second x 10 (rounded), from `frames` over `elapsed_us`
inline uint32_t DisplayFpsX10(uint32_t frames, uint64_t elapsed_us)
{
    if (!elapsed_us)
        return 0;
    return (uint32_t)(((uint64_t)frames * 10000000u + elapsed_us / 2) / elapsed_us);
}

// The pixel clock (Hz, rounded) that sends `frames` frames of `frame_clocks` clocks in `elapsed_us`
inline uint32_t DisplayPclkFromPeriods(uint32_t frame_clocks, uint32_t frames, uint64_t elapsed_us)
{
    if (!elapsed_us || !frames)
        return 0;
    return (uint32_t)(((uint64_t)frame_clocks * frames * 1000000u + elapsed_us / 2) / elapsed_us);
}

// A frame of `period_us` is what `pclk_hz` gives, within `tol_permille`
inline bool DisplayPeriodMatches(uint32_t frame_clocks, uint64_t period_us, uint32_t pclk_hz, uint32_t tol_permille)
{
    if (!pclk_hz)
        return false;
    uint64_t want = (uint64_t)frame_clocks * 1000000u;     // period_us x pclk_hz, if exact
    uint64_t got  = period_us * pclk_hz;
    uint64_t diff = got > want ? got - want : want - got;
    return diff * 1000u <= want * tol_permille;
}

// Phase 2: the frame made longer with a longer vertical front porch, at a fixed clock. The lines
// per frame (and so the fill spacing) don't change, only the blanking after them.

// The build's total lines a frame, clocks a line and lines a bounce fill (waveshare_rgb_lcd_port.h
// DISPLAY_V_TOTAL, DISPLAY_H_TOTAL, DISPLAY_BOUNCE_BUFFER_LINES; the port static_asserts them)
inline constexpr uint32_t DISPLAY_TIMING_V_TOTAL = 500;
inline constexpr uint32_t DISPLAY_TIMING_H_TOTAL = 820;
inline constexpr uint32_t DISPLAY_TIMING_FILL_LINES = 15;

// The most the LCD_CAM holds: lcd_vt_height (lines - 1) is a 10-bit field
inline constexpr uint32_t DISPLAY_V_TOTAL_HW_MAX = 1024;

// The front porch a total of `v_total` lines leaves (0 if it doesn't even hold the rest)
constexpr uint32_t DisplayVfpForTotal(uint32_t v_total, uint32_t vsw, uint32_t vbp, uint32_t v_res)
{
    return v_total > vsw + vbp + v_res ? v_total - vsw - vbp - v_res : 0;
}

// Frames per second x 10 (rounded) at a clock and frame size
constexpr uint32_t DisplayTimingFpsX10(uint32_t pclk_hz, uint32_t h_total, uint32_t v_total)
{
    return h_total && v_total
               ? (uint32_t)(((uint64_t)pclk_hz * 10u + (uint64_t)h_total * v_total / 2) / ((uint64_t)h_total * v_total))
               : 0;
}

// The total lines that give `fps_x10` (rounded), kept between `min_total` and the hardware's most
inline uint32_t DisplayVTotalForFpsX10(uint32_t pclk_hz, uint32_t h_total, uint32_t fps_x10, uint32_t min_total)
{
    uint64_t div   = (uint64_t)h_total * fps_x10;
    uint64_t lines = div ? ((uint64_t)pclk_hz * 10u + div / 2) / div : DISPLAY_V_TOTAL_HW_MAX;
    if (lines > DISPLAY_V_TOTAL_HW_MAX)
        lines = DISPLAY_V_TOTAL_HW_MAX;
    if (lines < min_total)
        lines = min_total;
    return (uint32_t)lines;
}

// The total lines (rounded) that send `frames` frames in `elapsed_us` at `pclk_hz`
inline uint32_t DisplayVTotalFromPeriods(uint32_t h_total, uint32_t pclk_hz, uint32_t frames, uint64_t elapsed_us)
{
    uint64_t div = (uint64_t)frames * h_total * 1000000u;
    if (!div || !pclk_hz)
        return 0;
    return (uint32_t)((elapsed_us * pclk_hz + div / 2) / div);
}

// Phase 3: the timing as a stored setting (DISPLAY_SET, display_config.h)

// The panel's lowest clock: below it (12 MHz and less, phase 1) it shows a self-test colour
inline constexpr uint32_t DISPLAY_PCLK_MIN_MHZ = 16;

// What a build allows: the clock from the panel's floor up to the build's own (fill_gap.h's
// deadline assumes at least the build's fill spacing, so never above it), the frame from the
// build's 500 lines up to the LCD_CAM's 1024. A build below the floor allows only its own clock.
struct DisplayTimingLimits
{
    uint8_t  default_mhz, min_mhz, max_mhz;
    uint16_t default_lines, min_lines, max_lines;
};

constexpr DisplayTimingLimits DisplayTimingLimitsFor(uint32_t build_mhz)
{
    return { (uint8_t)build_mhz, (uint8_t)(build_mhz < DISPLAY_PCLK_MIN_MHZ ? build_mhz : DISPLAY_PCLK_MIN_MHZ),
             (uint8_t)build_mhz, (uint16_t)DISPLAY_TIMING_V_TOTAL, (uint16_t)DISPLAY_TIMING_V_TOTAL,
             (uint16_t)DISPLAY_V_TOTAL_HW_MAX };
}

constexpr bool DisplayTimingValid(const DisplayTimingLimits &l, uint32_t mhz, uint32_t lines)
{
    return mhz >= l.min_mhz && mhz <= l.max_mhz && lines >= l.min_lines && lines <= l.max_lines;
}

// A step of an experiment's schedule: a clock and a frame size, held `hold_s` seconds
struct PclkStep
{
    uint8_t  mhz;
    uint8_t  hold_s;
    uint16_t lines = DISPLAY_TIMING_V_TOTAL;
};

// Phase 1's schedule: steps down from 20 MHz and back up (8 s each), then a quick
// 20/8 MHz toggle (1 s each) to show switch artifacts; then it repeats. Every step changes
// the clock. Never above 20 MHz: the fill gaps' deadline assumes at least the build's spacing
// (fill_gap.h FILL_GAP_PERIOD_US).

inline constexpr PclkStep PCLK_SCHEDULE[] = {
    { 20, 8 }, { 16, 8 }, { 12, 8 }, { 10, 8 }, { 8, 8 }, { 6, 8 }, { 20, 8 },
    { 8, 1 }, { 20, 1 }, { 8, 1 }, { 20, 1 }, { 8, 1 }, { 20, 1 },
    { 8, 1 }, { 20, 1 }, { 8, 1 }, { 20, 1 }, { 8, 1 },
};
inline constexpr int PCLK_SCHEDULE_LEN = (int)(sizeof(PCLK_SCHEDULE) / sizeof(PCLK_SCHEDULE[0]));

// Phase 2's schedule (VBLANK_CYCLE): at 20 MHz, then at 16 MHz (the panel's proven clocks), the
// frame grows 500 -> 600 -> 750 -> 900 -> 1024 lines and back to 500 (8 s each); then a quick
// 500/1000-line toggle at 20 MHz (1 s each); then it repeats. Each step changes exactly one of
// the clock and the lines, so a log line names one switch.
inline constexpr PclkStep VBLANK_SCHEDULE[] = {
    { 20, 8, 500 }, { 20, 8, 600 }, { 20, 8, 750 }, { 20, 8, 900 }, { 20, 8, 1024 }, { 20, 8, 500 },
    { 16, 8, 500 }, { 16, 8, 600 }, { 16, 8, 750 }, { 16, 8, 900 }, { 16, 8, 1024 }, { 16, 8, 500 },
    { 20, 1, 500 }, { 20, 1, 1000 }, { 20, 1, 500 }, { 20, 1, 1000 }, { 20, 1, 500 }, { 20, 1, 1000 },
    { 20, 1, 500 }, { 20, 1, 1000 }, { 20, 1, 500 }, { 20, 1, 1000 },
};
inline constexpr int VBLANK_SCHEDULE_LEN = (int)(sizeof(VBLANK_SCHEDULE) / sizeof(VBLANK_SCHEDULE[0]));

// Step `i` of a schedule (any i >= 0: schedules repeat)
inline PclkStep PclkScheduleStep(int i)
{
    return PCLK_SCHEDULE[i % PCLK_SCHEDULE_LEN];
}

inline PclkStep VblankScheduleStep(int i)
{
    return VBLANK_SCHEDULE[i % VBLANK_SCHEDULE_LEN];
}

// One hold's fill stats, from the once-a-second samples (screen_stats_t)
struct FillHoldStats
{
    uint32_t seconds = 0;
    uint32_t fill_max_us = 0;
    uint32_t end_max_us = 0;
    uint32_t missed = 0;
    uint32_t overruns = 0;
    uint32_t seq_breaks = 0;
    uint32_t buffer_mismatches = 0;

    void Reset() { *this = FillHoldStats(); }

    void Add(uint32_t fill_max, uint32_t end_max, uint32_t missed_, uint32_t overruns_,
             uint32_t seq_breaks_, uint32_t buffer_mismatches_)
    {
        seconds++;
        if (fill_max > fill_max_us)
            fill_max_us = fill_max;
        if (end_max > end_max_us)
            end_max_us = end_max;
        missed            += missed_;
        overruns          += overruns_;
        seq_breaks        += seq_breaks_;
        buffer_mismatches += buffer_mismatches_;
    }
};
