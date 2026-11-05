#ifndef _RGB_LCD_H_
#define _RGB_LCD_H_

#include "esp_log.h"
#include "esp_check.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////// Please update the following configuration according to your LCD spec //////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
#define DISPLAY_H_RES               (800)
#define DISPLAY_V_RES               (480)
// Pixel clock: 25 MHz (~61 fps) unless the build sets PIXEL_CLOCK_MHZ (-DCB_PIXEL_CLOCK_MHZ=n,
// main/CMakeLists.txt; whole MHz). A slower clock gives each fill a bigger budget but fewer fps.
#ifdef PIXEL_CLOCK_MHZ
#define DISPLAY_PIXEL_CLOCK_HZ      (PIXEL_CLOCK_MHZ * 1000 * 1000)
#else
#define DISPLAY_PIXEL_CLOCK_HZ      (25 * 1000 * 1000)
#endif
#define DISPLAY_HSYNC_PULSE         (4)
#define DISPLAY_HSYNC_BACK_PORCH    (8)
#define DISPLAY_HSYNC_FRONT_PORCH   (8)
#define DISPLAY_H_TOTAL             (DISPLAY_H_RES + DISPLAY_HSYNC_PULSE + DISPLAY_HSYNC_BACK_PORCH + DISPLAY_HSYNC_FRONT_PORCH)
#define DISPLAY_VSYNC_PULSE         (4)
#define DISPLAY_VSYNC_BACK_PORCH    (8)
#define DISPLAY_VSYNC_FRONT_PORCH   (8)
#define DISPLAY_V_TOTAL             (DISPLAY_V_RES + DISPLAY_VSYNC_PULSE + DISPLAY_VSYNC_BACK_PORCH + DISPLAY_VSYNC_FRONT_PORCH)
#define DISPLAY_DATA_WIDTH          (16)
#define DISPLAY_BOUNCE_BUFFER_LINES (15)
#define DISPLAY_BOUNCE_BUFFER_SIZE  (DISPLAY_H_RES * DISPLAY_BOUNCE_BUFFER_LINES)
// Time the display takes to send one bounce buffer: a fill must finish within this.
#define DISPLAY_BOUNCE_BUDGET_US    (DISPLAY_BOUNCE_BUFFER_LINES * DISPLAY_H_TOTAL / (DISPLAY_PIXEL_CLOCK_HZ / 1000000))
#define DISPLAY_IO_RGB_DISP         ((gpio_num_t)(-1))
#define DISPLAY_IO_RGB_VSYNC        (GPIO_NUM_3)
#define DISPLAY_IO_RGB_HSYNC        (GPIO_NUM_46)
#define DISPLAY_IO_RGB_DE           (GPIO_NUM_5)
#define DISPLAY_IO_RGB_PCLK         (GPIO_NUM_7)
#define DISPLAY_IO_RGB_DATA0        (GPIO_NUM_14)
#define DISPLAY_IO_RGB_DATA1        (GPIO_NUM_38)
#define DISPLAY_IO_RGB_DATA2        (GPIO_NUM_18)
#define DISPLAY_IO_RGB_DATA3        (GPIO_NUM_17)
#define DISPLAY_IO_RGB_DATA4        (GPIO_NUM_10)
#define DISPLAY_IO_RGB_DATA5        (GPIO_NUM_39)
#define DISPLAY_IO_RGB_DATA6        (GPIO_NUM_0)
#define DISPLAY_IO_RGB_DATA7        (GPIO_NUM_45)
#define DISPLAY_IO_RGB_DATA8        (GPIO_NUM_48)
#define DISPLAY_IO_RGB_DATA9        (GPIO_NUM_47)
#define DISPLAY_IO_RGB_DATA10       (GPIO_NUM_21)
#define DISPLAY_IO_RGB_DATA11       (GPIO_NUM_1)
#define DISPLAY_IO_RGB_DATA12       (GPIO_NUM_2)
#define DISPLAY_IO_RGB_DATA13       (GPIO_NUM_42)
#define DISPLAY_IO_RGB_DATA14       (GPIO_NUM_41)
#define DISPLAY_IO_RGB_DATA15       (GPIO_NUM_40)
// Drive strength of the panel pins above (HSYNC, VSYNC, DE, PCLK, data). At the default (2)
// their fast edges jam the 2.4 GHz radio (pings of 100s of ms, lost packets); 0 fixes it
// and the picture is fine (wip/wip-wifi-latency.md).
#define DISPLAY_PIN_DRIVE_CAP       (GPIO_DRIVE_CAP_0)

// Called from the vsync ISR once a display_set_framebuffer() swap has completed
// and the previously-displayed buffer is safe to draw into again.
// freed_fb: the buffer that was just replaced by the display.
// Returns true if a higher-priority task was woken (pass to portYIELD_FROM_ISR).
typedef bool (*PFNBUFFERREADY)(uint16_t *freed_fb);

// Initialize the display.  Allocates two PSRAM frame buffers and starts the
// RGB DMA engine.  ready_cb (may be NULL) is called from the vsync ISR after
// each display_set_framebuffer() swap completes.
esp_err_t display_init(PFNBUFFERREADY ready_cb);

// Bounce-buffer mode: no frame buffers. The driver calls fill_cb from the
// LCD DMA ISR whenever it needs more pixels:
//   dest:  where to write `count` full lines (DISPLAY_H_RES pixels each, RGB565)
//   y:     first line to fill. y == 0 means a new frame is starting.
//   count: number of lines, always DISPLAY_BOUNCE_BUFFER_LINES
// Lines arrive in order, a frame at a time. fill_cb runs in ISR context: keep
// it fast, put it in IRAM, and only touch data in internal RAM or PSRAM.
// Returns true if a higher-priority task was woken.
typedef bool (*PFNFILLLINES)(uint16_t *dest, int y, int count, void *ctx);

// Initialize the display in bounce-buffer mode (see PFNFILLLINES).
// Use instead of display_init(). The frame buffer functions below don't
// apply in this mode.
esp_err_t display_init_bounce(PFNFILLLINES fill_cb, void *ctx);

// Backlight on/off (CH422G line, no PWM). Needs ch422g_init().
esp_err_t display_enable_backlight(bool enable);

// Return both frame buffer pointers (order matches driver allocation).
// Use at startup to populate both buffers before handing control to the
// display_set_framebuffer() / PFNBUFFERREADY cycle.
void display_get_frame_buffers(uint16_t **fb0, uint16_t **fb1);

// Request that fb becomes the displayed buffer at the next vertical retrace.
// fb must be one of the two pointers returned by display_get_frame_buffers().
// When the swap completes the PFNBUFFERREADY callback fires with the old buffer.
void display_set_framebuffer(uint16_t *fb);


#endif
