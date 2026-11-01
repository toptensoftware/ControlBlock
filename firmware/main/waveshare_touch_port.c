#include "waveshare_touch_port.h"
#include "waveshare_rgb_lcd_port.h"
#include "esp_check.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_gt911.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c.h"

static const char *TAG = "waveshare_touch";

#define CH422G_ADDR_SYS  0x24
#define CH422G_ADDR_OUT  0x38

#define MAX_POINTS CONFIG_ESP_LCD_TOUCH_MAX_POINTS

static esp_lcd_touch_handle_t s_tp         = NULL;
static PFNTOUCHED             s_touched_cb = NULL;

typedef struct {
    bool     active;
    uint16_t x, y;
} touch_point_state_t;

static touch_point_state_t s_state[MAX_POINTS];

static void touch_reset(i2c_master_bus_handle_t bus)
{
    // Configure CH422G to output mode, then toggle the touch reset line.
    // bit 1 = touch RST (0=assert), bit 2 = backlight, bits 3,4 = always-on enables.
    // Use 0x1C/0x1E to match display_enable_backlight's IO4 usage.
    i2c_master_dev_handle_t dev_sys, dev_out;
    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.scl_speed_hz    = 400000;
    dev_cfg.device_address  = CH422G_ADDR_SYS;
    i2c_master_bus_add_device(bus, &dev_cfg, &dev_sys);
    dev_cfg.device_address = CH422G_ADDR_OUT;
    i2c_master_bus_add_device(bus, &dev_cfg, &dev_out);

    uint8_t buf = 0x01;
    i2c_master_transmit(dev_sys, &buf, 1, -1);
    buf = 0x1C;  // assert touch reset (bit1=0), backlight+IO4 on (bits 2,3,4)
    i2c_master_transmit(dev_out, &buf, 1, -1);
    vTaskDelay(pdMS_TO_TICKS(100));

    gpio_set_level(TOUCH_RESET_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(100));

    buf = 0x1E;  // release touch reset (bit1=1), backlight+IO4 on (bits 2,3,4)
    i2c_master_transmit(dev_out, &buf, 1, -1);
    vTaskDelay(pdMS_TO_TICKS(200));

    i2c_master_bus_rm_device(dev_sys);
    i2c_master_bus_rm_device(dev_out);
}

static void touch_task(void *arg)
{
    while (1) {
        esp_lcd_touch_read_data(s_tp);

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
                s_state[id] = (touch_point_state_t){ true, points[i].x, points[i].y };
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

        // 100 Hz poll, for smooth dragging. Polling faster than the GT911
        // updates is harmless: the driver keeps the last reading.
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t touch_init(PFNTOUCHED touched_cb)
{
    s_touched_cb = touched_cb;

    gpio_config_t io_conf = {};
    io_conf.intr_type    = GPIO_INTR_DISABLE;
    io_conf.pin_bit_mask = 1ULL << TOUCH_RESET_GPIO;
    io_conf.mode         = GPIO_MODE_OUTPUT;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en   = GPIO_PULLUP_DISABLE;
    ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "GPIO config failed");

    ESP_LOGI(TAG, "Resetting touch controller");
    touch_reset(i2c_get_bus());

    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_config = {};
    tp_io_config.scl_speed_hz                = 100000;
    tp_io_config.dev_addr                    = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS;
    tp_io_config.control_phase_bytes         = 1;
    tp_io_config.dc_bit_offset               = 0;
    tp_io_config.lcd_cmd_bits                = 16;
    tp_io_config.flags.disable_control_phase = 1;

    ESP_LOGI(TAG, "Creating touch panel I2C IO");
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_i2c(i2c_get_bus(), &tp_io_config, &tp_io_handle),
        TAG, "Touch panel IO init failed");

    ESP_LOGI(TAG, "Initializing GT911 touch controller");
    esp_lcd_touch_config_t tp_cfg = {};
    tp_cfg.x_max        = 800;
    tp_cfg.y_max        = 480;
    tp_cfg.rst_gpio_num = (gpio_num_t)TOUCH_PIN_NUM_RST;
    tp_cfg.int_gpio_num = (gpio_num_t)TOUCH_PIN_NUM_INT;
    ESP_RETURN_ON_ERROR(
        esp_lcd_touch_new_i2c_gt911(tp_io_handle, &tp_cfg, &s_tp),
        TAG, "GT911 init failed");

    BaseType_t ret = xTaskCreatePinnedToCore(
        touch_task, "touch", 4096, NULL,
        5, NULL, 1); // core 1
    return ret == pdPASS ? ESP_OK : ESP_FAIL;
}
