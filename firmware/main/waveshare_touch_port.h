#ifndef _WAVESHARE_TOUCH_PORT_H_
#define _WAVESHARE_TOUCH_PORT_H_

#include "esp_err.h"
#include <stdint.h>

// GT911 INT line. Held low while the GT911 comes out of reset to select I2C
// address 0x5D, then released to an input (the GT911 drives it after that).
#define TOUCH_INT_GPIO          GPIO_NUM_4

// The GT911 component doesn't manage reset or INT: reset is on the CH422G
// and INT is handled in touch_init().
#define TOUCH_PIN_NUM_RST       (-1)
#define TOUCH_PIN_NUM_INT       (-1)

typedef enum {
    TOUCH_DOWN,
    TOUCH_MOVE,
    TOUCH_UP,
} TOUCH_EVENT_TYPE;

// Called from the touch polling task for each finger state change.
// id:   hardware track ID (0..CONFIG_ESP_LCD_TOUCH_MAX_POINTS-1)
// type: DOWN on first contact, MOVE on position change, UP on lift
// x, y: screen coordinates
typedef void (*PFNTOUCHED)(uint8_t id, TOUCH_EVENT_TYPE type, uint16_t x, uint16_t y);

// Initialize the GT911 touch controller and start the polling task.
// touched_cb is called from the touch task on each finger state change.
// Must be called after i2c_init() and ch422g_init().
esp_err_t touch_init(PFNTOUCHED touched_cb);

#endif // _WAVESHARE_TOUCH_PORT_H_
