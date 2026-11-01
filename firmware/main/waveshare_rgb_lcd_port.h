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
#define DISPLAY_PIXEL_CLOCK_HZ      (25 * 1000 * 1000)
#define DISPLAY_DATA_WIDTH          (16)
#define DISPLAY_BOUNCE_BUFFER_SIZE  (DISPLAY_H_RES * 15)
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

// Called from the vsync ISR once a display_set_framebuffer() swap has completed
// and the previously-displayed buffer is safe to draw into again.
// freed_fb: the buffer that was just replaced by the display.
// Returns true if a higher-priority task was woken (pass to portYIELD_FROM_ISR).
typedef bool (*PFNBUFFERREADY)(uint16_t *freed_fb);

// Initialize the display.  Allocates two PSRAM frame buffers and starts the
// RGB DMA engine.  ready_cb (may be NULL) is called from the vsync ISR after
// each display_set_framebuffer() swap completes.
esp_err_t display_init(PFNBUFFERREADY ready_cb);

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
