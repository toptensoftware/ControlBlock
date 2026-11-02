#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// CH422G IO expander on the shared I2C bus. Its EXIO0-7 outputs drive board
// control lines that aren't on plain GPIOs. One copy of the output byte is
// kept here so drivers can change their own line without disturbing others.
//
// Lines, per the Waveshare schematic:
#define CH422G_TP_RST     (1 << 1)   // touch reset, low = in reset
#define CH422G_BACKLIGHT  (1 << 2)   // LCD backlight enable
#define CH422G_LCD_RST    (1 << 3)   // LCD reset, low = in reset
#define CH422G_SD_CS      (1 << 4)   // SD card chip select, low = selected
#define CH422G_USB_SEL    (1 << 5)   // native USB / CAN mux, low = USB

// Set up the expander after i2c_init(): resets released, SD deselected,
// backlight off, native USB selected.
esp_err_t ch422g_init(void);

// Set (on) or clear the output lines in `mask`. Thread safe.
esp_err_t ch422g_set(uint8_t mask, bool on);
