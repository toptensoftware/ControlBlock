/*
 * SPDX-FileCopyrightText: 2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "waveshare_rgb_lcd_port.h"
#include "ch422g.h"

static const char *TAG = "waveshare_lcd";

static esp_lcd_panel_handle_t   s_panel        = NULL;

// Twin frame buffers managed by the RGB panel driver (allocated in PSRAM).
// s_fb[s_front_idx] is the buffer currently being scanned out; the other is
// the back (draw) buffer.  Pointers are retrieved after panel init via
// esp_lcd_rgb_panel_get_frame_buffer().
static uint16_t *s_fb[2]         = {NULL, NULL};
static volatile int  s_front_idx = 0;
static volatile bool s_swap_pending = false;

static PFNBUFFERREADY s_ready_cb  = NULL;

// Bounce-buffer mode: the user callback fills each bounce buffer directly.
static PFNFILLLINES s_fill_cb  = NULL;
static void        *s_fill_ctx = NULL;

static_assert(DISPLAY_V_RES % DISPLAY_BOUNCE_BUFFER_LINES == 0,
               "Bounce buffer must divide the screen into whole chunks");

// Called from the LCD DMA ISR each time a bounce buffer needs refilling.
// The bounce buffer is always a whole number of lines, so convert the
// driver's pixel position into a line number.
#if PCLK_CYCLE
static void note_fill(int y);               // the frame rate experiment's VSYNC_END position
#endif

IRAM_ATTR static bool on_bounce_empty(esp_lcd_panel_handle_t panel, void *bounce_buf,
                                      int pos_px, int len_bytes, void *user_ctx)
{
#if PCLK_CYCLE
    note_fill(pos_px / DISPLAY_H_RES);
#endif
    return s_fill_cb((uint16_t *)bounce_buf,
                     pos_px / DISPLAY_H_RES,
                     len_bytes / (DISPLAY_H_RES * (int)sizeof(uint16_t)),
                     s_fill_ctx);
}

// Called when the entire frame has been passed through the bounce buffer.
IRAM_ATTR static bool on_frame_buf_complete(esp_lcd_panel_handle_t panel,
                                             const esp_lcd_rgb_panel_event_data_t *edata,
                                             void *user_ctx)
{
    if (!s_swap_pending)
        return false;

    uint16_t *freed_fb = s_fb[s_front_idx];
    s_front_idx   ^= 1;
    s_swap_pending  = false;

    if (s_ready_cb) {
        return s_ready_cb(freed_fb);
    }
    return false;
}

#include "hal/lcd_ll.h"
#include "display_timing.h"
#if PCLK_CYCLE
#include "esp_timer.h"
#endif

static_assert(DISPLAY_TIMING_V_TOTAL == DISPLAY_V_TOTAL, "display_timing.h has the build's frame size");
static_assert(DISPLAY_TIMING_H_TOTAL == DISPLAY_H_TOTAL, "display_timing.h has the build's line length");
static_assert(DISPLAY_TIMING_FILL_LINES == DISPLAY_BOUNCE_BUFFER_LINES, "display_timing.h has the build's fill");

// The display timing at runtime (display_set_timing): the pending clock or frame size change. The
// driver calls on_vsync and then applies a clock asked for by esp_lcd_rgb_panel_set_pclk() in the
// same interrupt, so a change is applied at the first VSYNC_END that sees it pending. A new frame
// size is written here, in on_vsync: the driver has no runtime API for it. The frame rate
// experiment (PCLK_CYCLE) also stamps each VSYNC_END.
static portMUX_TYPE      s_vsync_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t          s_req_hz    = DISPLAY_PIXEL_CLOCK_HZ;  // the last asked for (before init: the panel's start)
static uint32_t          s_req_lines = DISPLAY_V_TOTAL;
static uint32_t          s_pending_hz;      // 0: none
static uint32_t          s_pending_lines;   // 0: none
static volatile uint32_t s_pclk_hz = DISPLAY_PIXEL_CLOCK_HZ;
#if PCLK_CYCLE
static display_vsync_t   s_vsync = { 0, 0, 0, DISPLAY_PIXEL_CLOCK_HZ, DISPLAY_V_TOTAL, 0, 0, 0, 0 };
#endif

// How a change of the lines is written (DISPLAY_PORCH_*): only the phase 4 experiment changes it
#if PCLK_CYCLE
static volatile uint8_t s_porch_variant = 0;
static volatile int64_t s_vsync_stamp_us;           // the last VSYNC_END, for the fill ISR
static volatile int64_t s_vsync_to_y30_us;          // from it to the y = 30 fill
void display_set_porch_variant(uint8_t variant)
{
    s_porch_variant = variant;
}

static bool     s_pending_relatch;  // under s_vsync_lock
static uint32_t s_ramp_target;      // phase 6: the lines a ramp is heading for (0: none); under s_vsync_lock

void display_relatch(void)
{
    portENTER_CRITICAL(&s_vsync_lock);
    s_pending_relatch = true;
    portEXIT_CRITICAL(&s_vsync_lock);
}

// The y = 30 fill comes once lines 0-14 have gone out: its time from VSYNC_END is the blanking
// after VSYNC_END plus ~15 lines, which says where in the blanking VSYNC_END fires
IRAM_ATTR static void note_fill(int y)
{
    if (y == 2 * DISPLAY_BOUNCE_BUFFER_LINES)
        s_vsync_to_y30_us = esp_timer_get_time() - s_vsync_stamp_us;
}
#else
static const uint8_t s_porch_variant = 0;
#endif

// The LCD_CAM's vertical timing, as the driver sets it at init (esp_lcd_panel_rgb.c rgb_panel_init)
// but with the front porch (or, in the phase 4 experiment, the back porch) that makes `lines` in
// all. lcd_update latches the new values (as lcd_ll_start does; the experiment can leave it out);
// at VSYNC_END the line counter is at a frame boundary, so a shorter total can't cut under it.
IRAM_ATTR static void set_vertical_total(uint32_t lines)
{
    DisplayVPorches p = DisplayPorchesForTotal(lines, DISPLAY_VSYNC_PULSE, DISPLAY_VSYNC_BACK_PORCH,
                                               DISPLAY_VSYNC_FRONT_PORCH, DISPLAY_V_RES,
                                               s_porch_variant & DISPLAY_PORCH_BACK);
    if (DISPLAY_VSYNC_PULSE + p.vbp > DISPLAY_VB_FRONT_HW_MAX)       // more than the back porch holds
        p = DisplayPorchesForTotal(lines, DISPLAY_VSYNC_PULSE, DISPLAY_VSYNC_BACK_PORCH,
                                   DISPLAY_VSYNC_FRONT_PORCH, DISPLAY_V_RES, false);
    lcd_ll_set_vertical_timing(&LCD_CAM, DISPLAY_VSYNC_PULSE, p.vbp, DISPLAY_V_RES, p.vfp);
    if (!(s_porch_variant & DISPLAY_PORCH_NO_LATCH))
        LCD_CAM.lcd_user.lcd_update = 1;
}

IRAM_ATTR static bool on_vsync(esp_lcd_panel_handle_t panel, const esp_lcd_rgb_panel_event_data_t *edata,
                               void *user_ctx)
{
#if PCLK_CYCLE
    int64_t now = esp_timer_get_time();
#endif
    portENTER_CRITICAL_ISR(&s_vsync_lock);
#if PCLK_CYCLE
    s_vsync.count++;
    s_vsync.prev_us = s_vsync.last_us;
    s_vsync.last_us = now;
    s_vsync_stamp_us = now;
    if (s_pending_lines && (s_porch_variant & DISPLAY_PORCH_RAMP)) {
        s_ramp_target   = s_pending_lines;      // phase 6: reached a step a frame, below
        s_pending_lines = 0;
    }
    bool applied = s_pending_lines || s_pending_hz || s_pending_relatch;
    if (s_ramp_target) {
        s_vsync.v_total = DisplayRampNext(s_vsync.v_total, s_ramp_target, DISPLAY_RAMP_LINES_PER_FRAME);
        set_vertical_total(s_vsync.v_total);
        if (s_vsync.v_total == s_ramp_target) {
            s_ramp_target = 0;
            applied = true;                     // the ramp's last step counts as the change
        }
    } else if (s_pending_relatch && !s_pending_lines) {
        set_vertical_total(s_vsync.v_total);    // the same values, latched again (phase 5)
    }
    s_pending_relatch = false;
    if (applied) {
        s_vsync.applied_count   = s_vsync.count;
        s_vsync.applied_us      = now;
        s_vsync.applied_prev_us = s_vsync.prev_us;
    }
    if (s_pending_lines)
        s_vsync.v_total = s_pending_lines;
    if (s_pending_hz)
        s_vsync.pclk_hz = s_pending_hz;
#endif
    if (s_pending_lines) {
        set_vertical_total(s_pending_lines);
        s_pending_lines = 0;
    }
    if (s_pending_hz) {
        s_pclk_hz    = s_pending_hz;
        s_pending_hz = 0;
    }
    portEXIT_CRITICAL_ISR(&s_vsync_lock);
    return false;
}

esp_err_t display_set_timing(uint32_t pclk_hz, uint32_t v_total)
{
    if (!pclk_hz || pclk_hz > DISPLAY_PIXEL_CLOCK_HZ || v_total < DISPLAY_V_TOTAL || v_total > DISPLAY_V_TOTAL_HW_MAX)
        return ESP_ERR_INVALID_ARG;
    esp_err_t err = ESP_OK;
    portENTER_CRITICAL(&s_vsync_lock);
    if (!s_panel) {
        // Not running yet: display_init_common creates the panel with this timing
        s_pclk_hz = pclk_hz;
#if PCLK_CYCLE
        s_vsync.pclk_hz = pclk_hz;
        s_vsync.v_total = v_total;
#endif
    } else {
        if (pclk_hz != s_req_hz) {
            err = esp_lcd_rgb_panel_set_pclk(s_panel, pclk_hz);
            if (err == ESP_OK)
                s_pending_hz = pclk_hz;
        }
        if (err == ESP_OK && v_total != s_req_lines)
            s_pending_lines = v_total;
    }
    if (err == ESP_OK) {
        s_req_hz    = pclk_hz;
        s_req_lines = v_total;
    }
    portEXIT_CRITICAL(&s_vsync_lock);
    return err;
}

IRAM_ATTR uint32_t display_pixel_clock_hz(void)
{
    return s_pclk_hz;
}

#if PCLK_CYCLE
void display_get_vsync(display_vsync_t *v)
{
    portENTER_CRITICAL(&s_vsync_lock);
    *v = s_vsync;
    v->vsync_to_y30_us = s_vsync_to_y30_us;
    portEXIT_CRITICAL(&s_vsync_lock);
}
#endif

// Shared setup for both modes. With fill_cb set, the panel runs without
// frame buffers and fill_cb supplies every line. Otherwise the driver
// allocates two PSRAM frame buffers.
static esp_err_t display_init_common(PFNBUFFERREADY ready_cb, PFNFILLLINES fill_cb, void *fill_ctx)
{
    s_ready_cb  = ready_cb;
    s_fill_cb   = fill_cb;
    s_fill_ctx  = fill_ctx;

    // The timing display_set_timing() asked for before init (the stored setting), else the build's
    uint32_t fps_x10 = DisplayTimingFpsX10(s_req_hz, DISPLAY_H_TOTAL, s_req_lines);
    ESP_LOGI(TAG, "Install RGB LCD panel driver: pixel clock %lu.%03lu MHz, %lu lines (~%lu.%lu fps), "
                  "fill budget %lu us (build: %d MHz, %d lines)",
             (unsigned long)(s_req_hz / 1000000), (unsigned long)(s_req_hz / 1000 % 1000), (unsigned long)s_req_lines,
             (unsigned long)(fps_x10 / 10), (unsigned long)(fps_x10 % 10),
             (unsigned long)DisplayFillSpacingUs(DISPLAY_BOUNCE_BUFFER_LINES, DISPLAY_H_TOTAL, s_req_hz),
             DISPLAY_PIXEL_CLOCK_HZ / 1000000, DISPLAY_V_TOTAL);
    esp_lcd_rgb_panel_config_t panel_config = {};
    panel_config.clk_src                        = LCD_CLK_SRC_DEFAULT;
    panel_config.timings.pclk_hz                = s_req_hz;
    panel_config.timings.h_res                  = DISPLAY_H_RES;
    panel_config.timings.v_res                  = DISPLAY_V_RES;
    panel_config.timings.hsync_pulse_width       = DISPLAY_HSYNC_PULSE;
    panel_config.timings.hsync_back_porch        = DISPLAY_HSYNC_BACK_PORCH;
    panel_config.timings.hsync_front_porch       = DISPLAY_HSYNC_FRONT_PORCH;
    panel_config.timings.vsync_pulse_width       = DISPLAY_VSYNC_PULSE;
    panel_config.timings.vsync_back_porch        = DISPLAY_VSYNC_BACK_PORCH;
    panel_config.timings.vsync_front_porch       = DisplayVfpForTotal(s_req_lines, DISPLAY_VSYNC_PULSE,
                                                                      DISPLAY_VSYNC_BACK_PORCH, DISPLAY_V_RES);
    panel_config.timings.flags.pclk_active_neg   = 1;
    panel_config.data_width                      = DISPLAY_DATA_WIDTH;
    panel_config.bounce_buffer_size_px           = DISPLAY_BOUNCE_BUFFER_SIZE;
    if (fill_cb) {
        panel_config.flags.no_fb                 = 1;
    } else {
        panel_config.num_fbs                     = 2;
        panel_config.flags.fb_in_psram           = 1;
    }
    panel_config.hsync_gpio_num                  = DISPLAY_IO_RGB_HSYNC;
    panel_config.vsync_gpio_num                  = DISPLAY_IO_RGB_VSYNC;
    panel_config.de_gpio_num                     = DISPLAY_IO_RGB_DE;
    panel_config.pclk_gpio_num                   = DISPLAY_IO_RGB_PCLK;
    panel_config.disp_gpio_num                   = DISPLAY_IO_RGB_DISP;
    panel_config.data_gpio_nums[0]               = DISPLAY_IO_RGB_DATA0;
    panel_config.data_gpio_nums[1]               = DISPLAY_IO_RGB_DATA1;
    panel_config.data_gpio_nums[2]               = DISPLAY_IO_RGB_DATA2;
    panel_config.data_gpio_nums[3]               = DISPLAY_IO_RGB_DATA3;
    panel_config.data_gpio_nums[4]               = DISPLAY_IO_RGB_DATA4;
    panel_config.data_gpio_nums[5]               = DISPLAY_IO_RGB_DATA5;
    panel_config.data_gpio_nums[6]               = DISPLAY_IO_RGB_DATA6;
    panel_config.data_gpio_nums[7]               = DISPLAY_IO_RGB_DATA7;
    panel_config.data_gpio_nums[8]               = DISPLAY_IO_RGB_DATA8;
    panel_config.data_gpio_nums[9]               = DISPLAY_IO_RGB_DATA9;
    panel_config.data_gpio_nums[10]              = DISPLAY_IO_RGB_DATA10;
    panel_config.data_gpio_nums[11]              = DISPLAY_IO_RGB_DATA11;
    panel_config.data_gpio_nums[12]              = DISPLAY_IO_RGB_DATA12;
    panel_config.data_gpio_nums[13]              = DISPLAY_IO_RGB_DATA13;
    panel_config.data_gpio_nums[14]              = DISPLAY_IO_RGB_DATA14;
    panel_config.data_gpio_nums[15]              = DISPLAY_IO_RGB_DATA15;

    ESP_RETURN_ON_ERROR(esp_lcd_new_rgb_panel(&panel_config, &s_panel), TAG, "RGB panel create failed");

    // Weak drive on the panel pins (DISPLAY_PIN_DRIVE_CAP: slower edges keep their noise
    // off the Wi-Fi radio). The driver has just configured them, at the default (2).
    {
        const gpio_num_t pins[] = { panel_config.hsync_gpio_num, panel_config.vsync_gpio_num,
                                    panel_config.de_gpio_num, panel_config.pclk_gpio_num };
        for (gpio_num_t pin : pins)
            ESP_ERROR_CHECK(gpio_set_drive_capability(pin, DISPLAY_PIN_DRIVE_CAP));
        for (int i = 0; i < DISPLAY_DATA_WIDTH; i++)
            ESP_ERROR_CHECK(gpio_set_drive_capability((gpio_num_t)panel_config.data_gpio_nums[i],
                                                      DISPLAY_PIN_DRIVE_CAP));
        ESP_LOGI(TAG, "panel pins at drive capability %d", (int)DISPLAY_PIN_DRIVE_CAP);
    }

    // Register callbacks before init — the DMA engine starts on init.
    esp_lcd_rgb_panel_event_callbacks_t cbs = {};
    if (fill_cb)
        cbs.on_bounce_empty = on_bounce_empty;
    else
        cbs.on_frame_buf_complete = on_frame_buf_complete;
    cbs.on_vsync = on_vsync;            // runtime timing changes (display_set_timing)
    ESP_RETURN_ON_ERROR(esp_lcd_rgb_panel_register_event_callbacks(s_panel, &cbs, NULL), TAG, "RGB panel callbacks failed");

    ESP_LOGI(TAG, "Initialize RGB LCD panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "RGB panel init failed");

    if (!fill_cb) {
        // Retrieve the two PSRAM frame buffer pointers allocated by the driver.
        ESP_RETURN_ON_ERROR(esp_lcd_rgb_panel_get_frame_buffer(s_panel, 2, (void **)&s_fb[0], (void **)&s_fb[1]),
                            TAG, "Get frame buffers failed");
        ESP_LOGI(TAG, "Frame buffers: fb0=%p  fb1=%p", s_fb[0], s_fb[1]);
    }

    return ESP_OK;
}

esp_err_t display_init(PFNBUFFERREADY ready_cb)
{
    return display_init_common(ready_cb, NULL, NULL);
}

esp_err_t display_init_bounce(PFNFILLLINES fill_cb, void *ctx)
{
    assert(fill_cb);
    return display_init_common(NULL, fill_cb, ctx);
}

void display_get_frame_buffers(uint16_t **fb0, uint16_t **fb1)
{
    *fb0 = s_fb[0];
    *fb1 = s_fb[1];
}

void display_set_framebuffer(uint16_t *fb)
{
    // Passing an owned FB pointer to draw_bitmap causes the driver to flip its
    // internal cur_fb_index with no copy.  The bounce-buffer fill task picks
    // this up at the next frame boundary, so the switch is tear-free.
    assert(!s_swap_pending);
    assert(fb == s_fb[0] || fb == s_fb[1]);
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, DISPLAY_H_RES, DISPLAY_V_RES, fb);
    s_swap_pending = true;
}

// The backlight is locked on (as at power-up) unless BACKLIGHT_SWITCH is built in: switching
// it on corrupts PSRAM and drops the CH343P (CLAUDE.md Open Items, docs/backlight.md Hardware)
esp_err_t display_enable_backlight(bool enable)
{
#if BACKLIGHT_SWITCH
    return ch422g_set(CH422G_BACKLIGHT, enable);
#else
    (void)enable;
    return ESP_OK;
#endif
}
