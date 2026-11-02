#include "waveshare_touch_port.h"
#include "waveshare_rgb_lcd_port.h"
#include "esp_check.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_gt911.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c.h"
#include "ch422g.h"

static const char *TAG = "waveshare_touch";

#define MAX_POINTS     CONFIG_ESP_LCD_TOUCH_MAX_POINTS
#define POLL_PERIOD_MS 10       // 100 Hz, for smooth dragging

static esp_lcd_touch_handle_t s_tp         = NULL;
static PFNTOUCHED             s_touched_cb = NULL;

typedef struct {
    bool     active;
    uint16_t x, y;
} touch_point_state_t;

static touch_point_state_t s_state[MAX_POINTS];

// Reset the GT911 with INT held low, which selects I2C address 0x5D, then
// release INT so the GT911 can drive it.
static esp_err_t touch_reset(void)
{
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = 1ULL << TOUCH_INT_GPIO;
    io_conf.mode         = GPIO_MODE_OUTPUT;
    io_conf.pull_up_en   = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type    = GPIO_INTR_DISABLE;
    ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "INT as output");
    ESP_RETURN_ON_ERROR(gpio_set_level(TOUCH_INT_GPIO, 0), TAG, "INT low");

    ESP_RETURN_ON_ERROR(ch422g_set(CH422G_TP_RST, false), TAG, "assert reset");
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(ch422g_set(CH422G_TP_RST, true), TAG, "release reset");

    // GT911 latches the address within ~5 ms of reset release; keep INT low
    // past that, then let go of it.
    vTaskDelay(pdMS_TO_TICKS(20));
    io_conf.mode = GPIO_MODE_INPUT;
    ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "INT as input");

    // GT911 needs ~50 ms after reset before it answers on I2C
    vTaskDelay(pdMS_TO_TICKS(100));
    return ESP_OK;
}

static void touch_task(void *arg)
{
    TickType_t last_wake = xTaskGetTickCount();
    int        errors    = 0;

    while (1) {
        // Even rate, independent of how long the read takes
        xTaskDelayUntil(&last_wake, pdMS_TO_TICKS(POLL_PERIOD_MS));

        // On a failed read, keep the previous state rather than reporting
        // every finger as lifted.
        if (esp_lcd_touch_read_data(s_tp) != ESP_OK) {
            if (errors++ % 100 == 0)
                ESP_LOGW(TAG, "GT911 read failed (%d so far)", errors);
            continue;
        }

        esp_lcd_touch_point_data_t points[MAX_POINTS];
        uint8_t point_cnt = 0;
        esp_lcd_touch_get_data(s_tp, points, &point_cnt, MAX_POINTS);

        bool current[MAX_POINTS] = {};
        for (int i = 0; i < point_cnt; i++) {
            uint8_t id = points[i].track_id;
            if (id >= MAX_POINTS)
                continue;
            current[id] = true;

            if (!s_state[id].active) {
                s_state[id] = touch_point_state_t{ true, points[i].x, points[i].y };
                if (s_touched_cb)
                    s_touched_cb(id, TOUCH_DOWN, points[i].x, points[i].y);
            } else if (points[i].x != s_state[id].x || points[i].y != s_state[id].y) {
                s_state[id].x = points[i].x;
                s_state[id].y = points[i].y;
                if (s_touched_cb)
                    s_touched_cb(id, TOUCH_MOVE, points[i].x, points[i].y);
            }
        }

        for (int id = 0; id < MAX_POINTS; id++) {
            if (s_state[id].active && !current[id]) {
                uint16_t x = s_state[id].x, y = s_state[id].y;
                s_state[id].active = false;
                if (s_touched_cb)
                    s_touched_cb(id, TOUCH_UP, x, y);
            }
        }
    }
}

esp_err_t touch_init(PFNTOUCHED touched_cb)
{
    s_touched_cb = touched_cb;

    ESP_LOGI(TAG, "Resetting touch controller");
    ESP_RETURN_ON_ERROR(touch_reset(), TAG, "Touch reset failed");

    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_config = {};
    tp_io_config.scl_speed_hz                = I2C_MASTER_FREQ_HZ;
    tp_io_config.dev_addr                    = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS;
    tp_io_config.control_phase_bytes         = 1;
    tp_io_config.dc_bit_offset               = 0;
    tp_io_config.lcd_cmd_bits                = 16;
    tp_io_config.flags.disable_control_phase = 1;

    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_i2c(i2c_get_bus(), &tp_io_config, &tp_io_handle),
        TAG, "Touch panel IO init failed");

    esp_lcd_touch_config_t tp_cfg = {};
    tp_cfg.x_max        = DISPLAY_H_RES;
    tp_cfg.y_max        = DISPLAY_V_RES;
    tp_cfg.rst_gpio_num = (gpio_num_t)TOUCH_PIN_NUM_RST;
    tp_cfg.int_gpio_num = (gpio_num_t)TOUCH_PIN_NUM_INT;
    ESP_RETURN_ON_ERROR(
        esp_lcd_touch_new_i2c_gt911(tp_io_handle, &tp_cfg, &s_tp),
        TAG, "GT911 init failed");

    BaseType_t ret = xTaskCreatePinnedToCore(touch_task, "touch", 4096, NULL, 5, NULL, 1);
    return ret == pdPASS ? ESP_OK : ESP_FAIL;
}
