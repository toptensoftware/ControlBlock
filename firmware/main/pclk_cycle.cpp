// Frame rate experiment (pclk_cycle.h). Device only; built empty unless PCLK_CYCLE.

#include "pclk_cycle.h"

#if PCLK_CYCLE
#include <stdio.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "display_timing.h"
#include "status.h"

static const char *TAG = "pclk";

#define PCLK_START_DELAY_S  10          // seconds after boot before the first hold (Wi-Fi, USB settle)
#define PCLK_APPLY_WAIT_US  500000      // give up on a switch after this (a frame is <= ~68 ms at 6 MHz)
#define PCLK_MATCH_PERMILLE 20          // a frame period "matches" a timing within 2%

// Phase 1 steps the clock (PCLK_SCHEDULE); phase 2 (VBLANK_CYCLE) the clock and the frame size
#if VBLANK_CYCLE
#define SCHEDULE_NAME "vblank"
static const int SCHEDULE_LEN = VBLANK_SCHEDULE_LEN;
static PclkStep schedule_step(int i) { return VblankScheduleStep(i); }
#else
#define SCHEDULE_NAME "pclk"
static const int SCHEDULE_LEN = PCLK_SCHEDULE_LEN;
static PclkStep schedule_step(int i) { return PclkScheduleStep(i); }
#endif

static uint32_t        s_wait_s = PCLK_START_DELAY_S;
static int             s_step   = -1;   // index into the schedule (counts up; the schedule repeats)
static uint32_t        s_hz     = DISPLAY_PIXEL_CLOCK_HZ;
static uint32_t        s_lines  = DISPLAY_V_TOTAL;
static FillHoldStats   s_hold;
static display_vsync_t s_hold_start;    // the VSYNC_END the hold's frames are counted from

static uint32_t step_hz(PclkStep s)
{
    return (uint32_t)s.mhz * 1000000u;
}

static uint32_t vfp(uint32_t lines)
{
    return DisplayVfpForTotal(lines, DISPLAY_VSYNC_PULSE, DISPLAY_VSYNC_BACK_PORCH, DISPLAY_V_RES);
}

// "x.yyy" MHz
static const char *mhz_text(char *buf, size_t n, uint32_t hz)
{
    snprintf(buf, n, "%lu.%03lu", (unsigned long)(hz / 1000000), (unsigned long)(hz / 1000 % 1000));
    return buf;
}

// "x.y" from a value x 10
static const char *x10_text(char *buf, size_t n, uint32_t v)
{
    snprintf(buf, n, "%lu.%lu", (unsigned long)(v / 10), (unsigned long)(v % 10));
    return buf;
}

// The timing on the status bar, to tell which rate the screen is at
static void show_rate(void)
{
    char text[64], fps[12];
    snprintf(text, sizeof(text), "%lu MHz, vfp %lu lines, %s fps", (unsigned long)(s_hz / 1000000),
             (unsigned long)vfp(s_lines), x10_text(fps, sizeof(fps), DisplayTimingFpsX10(s_hz, DISPLAY_H_TOTAL, s_lines)));
    status_set_left(text);
}

static const char *period_judgement(uint64_t period_us, uint32_t from_hz, uint32_t from_lines, uint32_t to_hz,
                                    uint32_t to_lines)
{
    if (DisplayPeriodMatches(DISPLAY_H_TOTAL * to_lines, period_us, to_hz, PCLK_MATCH_PERMILLE))
        return "new";
    if (DisplayPeriodMatches(DISPLAY_H_TOTAL * from_lines, period_us, from_hz, PCLK_MATCH_PERMILLE))
        return "old";
    return "neither";
}

// What `frames` frames in `elapsed_us` measure: the clock if the lines are as asked, and the
// lines if the clock is
static void measured_text(char *buf, size_t n, uint32_t frames, uint64_t elapsed_us)
{
    char mhz[16];
    snprintf(buf, n, "pclk %s MHz at %lu lines, %lu lines at %lu MHz",
             mhz_text(mhz, sizeof(mhz), DisplayPclkFromPeriods(DISPLAY_H_TOTAL * s_lines, frames, elapsed_us)),
             (unsigned long)s_lines,
             (unsigned long)DisplayVTotalFromPeriods(DISPLAY_H_TOTAL, s_hz, frames, elapsed_us),
             (unsigned long)(s_hz / 1000000));
}

// The hold that just ended: fps and the timing from its VSYNC stamps, and its fill stats
static void log_hold(void)
{
    display_vsync_t v;
    display_get_vsync(&v);
    uint32_t frames  = v.count - s_hold_start.count;
    uint64_t elapsed = (uint64_t)(v.last_us - s_hold_start.last_us);
    char want[16], fps[12], expect[12], measured[112];
    measured_text(measured, sizeof(measured), frames, elapsed);
    ESP_LOGI(TAG, "hold %s MHz, %lu lines (vfp %lu): %lu s, %lu frames, %s fps (expect %s), measured %s | "
                  "fill max %lu us, end max %lu/%lu us, %lu missed, %lu over | %lu seq breaks, %lu buffer mismatches",
             mhz_text(want, sizeof(want), s_hz), (unsigned long)s_lines, (unsigned long)vfp(s_lines),
             (unsigned long)s_hold.seconds, (unsigned long)frames,
             x10_text(fps, sizeof(fps), DisplayFpsX10(frames, elapsed)),
             x10_text(expect, sizeof(expect), DisplayTimingFpsX10(s_hz, DISPLAY_H_TOTAL, s_lines)), measured,
             (unsigned long)s_hold.fill_max_us, (unsigned long)s_hold.end_max_us,
             (unsigned long)DisplayFillSpacingUs(DISPLAY_BOUNCE_BUFFER_LINES, DISPLAY_H_TOTAL, s_hz),
             (unsigned long)s_hold.missed, (unsigned long)s_hold.overruns,
             (unsigned long)s_hold.seq_breaks, (unsigned long)s_hold.buffer_mismatches);
}

// Ask for the step's clock or frame size (a step changes one), wait for the VSYNC_END that
// applies it and the first whole frame after it, and log the latency and the frame periods either
// side. Starts the next hold's frame count there.
static void switch_to(PclkStep step)
{
    uint32_t from_hz = s_hz, from_lines = s_lines;
    uint32_t hz = step_hz(step), lines = step.lines;
    display_vsync_t v0, v;
    display_get_vsync(&v0);
    int64_t t_set = esp_timer_get_time();
    esp_err_t err = screen_set_timing(hz, lines);     // changes only what differs
    s_hz    = hz;
    s_lines = lines;
    show_rate();

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set %lu Hz, %lu lines failed: %s", (unsigned long)hz, (unsigned long)lines,
                 esp_err_to_name(err));
        display_get_vsync(&s_hold_start);
        return;
    }
    // Poll every tick (10 ms, shorter than any frame): applied, then one whole frame after it
    do {
        vTaskDelay(1);
        display_get_vsync(&v);
    } while ((v.applied_count == v0.applied_count || v.count < v.applied_count + 1) &&
             esp_timer_get_time() - t_set < PCLK_APPLY_WAIT_US);
    if (v.applied_count == v0.applied_count) {
        ESP_LOGE(TAG, "%lu MHz %lu lines -> %lu MHz %lu lines: not applied after %d ms",
                 (unsigned long)(from_hz / 1000000), (unsigned long)from_lines, (unsigned long)(hz / 1000000),
                 (unsigned long)lines, PCLK_APPLY_WAIT_US / 1000);
        s_hold_start = v;
        return;
    }

    uint64_t before = (uint64_t)(v.applied_us - v.applied_prev_us);    // ended at the applying VSYNC_END
    uint64_t after  = (uint64_t)(v.last_us - v.prev_us);               // a whole frame since
    uint32_t lat_us = (uint32_t)(v.applied_us - t_set);
    char measured[112], lat[12];
    measured_text(measured, sizeof(measured), 1, after);
    ESP_LOGI(TAG, "%lu MHz %lu lines (vfp %lu) -> %lu MHz %lu lines (vfp %lu): applied %s ms after the call "
                  "(VSYNC %lu after it); frame before %lu us (%s), after %lu us (%s) = %s",
             (unsigned long)(from_hz / 1000000), (unsigned long)from_lines, (unsigned long)vfp(from_lines),
             (unsigned long)(hz / 1000000), (unsigned long)lines, (unsigned long)vfp(lines),
             x10_text(lat, sizeof(lat), lat_us / 100), (unsigned long)(v.applied_count - v0.count),
             (unsigned long)before, period_judgement(before, from_hz, from_lines, hz, lines),
             (unsigned long)after, period_judgement(after, from_hz, from_lines, hz, lines), measured);

    // Count the hold's frames from the applying VSYNC_END: all at the new timing
    s_hold_start         = v;
    s_hold_start.count   = v.applied_count;
    s_hold_start.last_us = v.applied_us;
}

void pclk_cycle_second(const screen_stats_t *st)
{
    if (s_wait_s) {
        if (--s_wait_s)
            return;
        ESP_LOGI(TAG, "frame rate experiment (%s): %d steps, repeating; watch dark and mid-grey areas, and "
                      "swipe at each rate", SCHEDULE_NAME, SCHEDULE_LEN);
        s_step = 0;
        display_get_vsync(&s_hold_start);
        s_hz    = s_hold_start.pclk_hz;          // the timing in effect (a stored one, if any)
        s_lines = s_hold_start.v_total;
        s_hold.Reset();
        PclkStep first = schedule_step(0);
        if (step_hz(first) != s_hz || first.lines != s_lines)
            switch_to(first);
        else
            show_rate();
        return;
    }

    s_hold.Add(st->max_fill_us, st->max_end_us, st->missed, st->overruns, st->seq_breaks, st->buffer_mismatches);
    if (s_hold.seconds < schedule_step(s_step).hold_s)
        return;

    log_hold();
    s_step = (s_step + 1) % SCHEDULE_LEN;
    s_hold.Reset();
    switch_to(schedule_step(s_step));
}
#endif
