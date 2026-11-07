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
IRAM_ATTR static bool on_bounce_empty(esp_lcd_panel_handle_t panel, void *bounce_buf,
                                      int pos_px, int len_bytes, void *user_ctx)
{
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

// Shared setup for both modes. With fill_cb set, the panel runs without
// frame buffers and fill_cb supplies every line. Otherwise the driver
// allocates two PSRAM frame buffers.
static esp_err_t display_init_common(PFNBUFFERREADY ready_cb, PFNFILLLINES fill_cb, void *fill_ctx)
{
    s_ready_cb  = ready_cb;
    s_fill_cb   = fill_cb;
    s_fill_ctx  = fill_ctx;

    ESP_LOGI(TAG, "Install RGB LCD panel driver: pixel clock %d MHz (~%d.%d fps), fill budget %d us",
             DISPLAY_PIXEL_CLOCK_HZ / 1000000,
             DISPLAY_PIXEL_CLOCK_HZ / (DISPLAY_H_TOTAL * DISPLAY_V_TOTAL),
             DISPLAY_PIXEL_CLOCK_HZ / (DISPLAY_H_TOTAL * DISPLAY_V_TOTAL / 10) % 10,
             DISPLAY_BOUNCE_BUDGET_US);
    esp_lcd_rgb_panel_config_t panel_config = {};
    panel_config.clk_src                        = LCD_CLK_SRC_DEFAULT;
    panel_config.timings.pclk_hz                = DISPLAY_PIXEL_CLOCK_HZ;
    panel_config.timings.h_res                  = DISPLAY_H_RES;
    panel_config.timings.v_res                  = DISPLAY_V_RES;
    panel_config.timings.hsync_pulse_width       = DISPLAY_HSYNC_PULSE;
    panel_config.timings.hsync_back_porch        = DISPLAY_HSYNC_BACK_PORCH;
    panel_config.timings.hsync_front_porch       = DISPLAY_HSYNC_FRONT_PORCH;
    panel_config.timings.vsync_pulse_width       = DISPLAY_VSYNC_PULSE;
    panel_config.timings.vsync_back_porch        = DISPLAY_VSYNC_BACK_PORCH;
    panel_config.timings.vsync_front_porch       = DISPLAY_VSYNC_FRONT_PORCH;
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
