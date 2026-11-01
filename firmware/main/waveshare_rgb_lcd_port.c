/*
 * SPDX-FileCopyrightText: 2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "waveshare_rgb_lcd_port.h"
#include "i2c.h"

static const char *TAG = "waveshare_lcd";

// CH422G I2C GPIO expander — address 0x24 selects the system-parameter register,
// address 0x38 selects the output register (CH422G encodes register in the device address).
#define CH422G_ADDR_SYS  0x24
#define CH422G_ADDR_OUT  0x38

static i2c_master_dev_handle_t  s_i2c_sys      = NULL;
static i2c_master_dev_handle_t  s_i2c_out      = NULL;
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

_Static_assert(DISPLAY_V_RES % DISPLAY_BOUNCE_BUFFER_LINES == 0,
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

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.scl_speed_hz    = I2C_MASTER_FREQ_HZ;
    dev_cfg.device_address  = CH422G_ADDR_SYS;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(i2c_get_bus(), &dev_cfg, &s_i2c_sys), TAG, "CH422G sys device add failed");
    dev_cfg.device_address = CH422G_ADDR_OUT;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(i2c_get_bus(), &dev_cfg, &s_i2c_out), TAG, "CH422G out device add failed");

    ESP_LOGI(TAG, "Install RGB LCD panel driver");
    esp_lcd_rgb_panel_config_t panel_config = {};
    panel_config.clk_src                        = LCD_CLK_SRC_DEFAULT;
    panel_config.timings.pclk_hz                = DISPLAY_PIXEL_CLOCK_HZ;
    panel_config.timings.h_res                  = DISPLAY_H_RES;
    panel_config.timings.v_res                  = DISPLAY_V_RES;
    panel_config.timings.hsync_pulse_width       = 4;
    panel_config.timings.hsync_back_porch        = 8;
    panel_config.timings.hsync_front_porch       = 8;
    panel_config.timings.vsync_pulse_width       = 4;
    panel_config.timings.vsync_back_porch        = 8;
    panel_config.timings.vsync_front_porch       = 8;
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

    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&panel_config, &s_panel));

    // Register callbacks before init — the DMA engine starts on init.
    esp_lcd_rgb_panel_event_callbacks_t cbs = {};
    if (fill_cb)
        cbs.on_bounce_empty = on_bounce_empty;
    else
        cbs.on_frame_buf_complete = on_frame_buf_complete;
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_register_event_callbacks(s_panel, &cbs, NULL));

    ESP_LOGI(TAG, "Initialize RGB LCD panel");
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));

    if (!fill_cb) {
        // Retrieve the two PSRAM frame buffer pointers allocated by the driver.
        ESP_ERROR_CHECK(esp_lcd_rgb_panel_get_frame_buffer(s_panel, 2, (void **)&s_fb[0], (void **)&s_fb[1]));
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

esp_err_t display_enable_backlight(bool enable)
{
    uint8_t buf = 0x01;
    ESP_RETURN_ON_ERROR(i2c_master_transmit(s_i2c_sys, &buf, 1, -1), TAG, "BL: CH422G sys write failed");
    buf = enable ? 0x1E : 0x1A;
    ESP_RETURN_ON_ERROR(i2c_master_transmit(s_i2c_out, &buf, 1, -1), TAG, "BL: CH422G out write failed");
    return ESP_OK;
}
