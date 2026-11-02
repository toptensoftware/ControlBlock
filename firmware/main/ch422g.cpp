#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_check.h"
#include "driver/i2c_master.h"
#include "i2c.h"
#include "ch422g.h"

static const char *TAG = "ch422g";

// The CH422G picks its register by I2C address.
#define ADDR_SYS  0x24      // system parameters
#define ADDR_OUT  0x38      // EXIO0-7 output levels

#define SYS_IO_OUTPUT  0x01 // EXIO pins are outputs

#define INITIAL_OUT  (CH422G_TP_RST | CH422G_LCD_RST | CH422G_SD_CS)

static i2c_master_dev_handle_t s_sys;
static i2c_master_dev_handle_t s_out;
static SemaphoreHandle_t       s_lock;
static uint8_t                 s_out_val;

static esp_err_t write_reg(i2c_master_dev_handle_t dev, uint8_t val)
{
    return i2c_master_transmit(dev, &val, 1, I2C_MASTER_TIMEOUT_MS);
}

esp_err_t ch422g_init(void)
{
    if (s_lock)
        return ESP_OK;

    i2c_device_config_t cfg = {};
    cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    cfg.scl_speed_hz    = I2C_MASTER_FREQ_HZ;
    cfg.device_address = ADDR_SYS;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(i2c_get_bus(), &cfg, &s_sys), TAG, "add sys device");
    cfg.device_address = ADDR_OUT;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(i2c_get_bus(), &cfg, &s_out), TAG, "add out device");

    ESP_RETURN_ON_ERROR(write_reg(s_sys, SYS_IO_OUTPUT), TAG, "set output mode");
    s_out_val = INITIAL_OUT;
    ESP_RETURN_ON_ERROR(write_reg(s_out, s_out_val), TAG, "write outputs");

    s_lock = xSemaphoreCreateMutex();
    return s_lock ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t ch422g_set(uint8_t mask, bool on)
{
    if (!s_lock)
        return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint8_t val = on ? (s_out_val | mask) : (s_out_val & ~mask);
    esp_err_t err = write_reg(s_out, val);
    if (err == ESP_OK)
        s_out_val = val;
    xSemaphoreGive(s_lock);

    ESP_RETURN_ON_ERROR(err, TAG, "write outputs");
    return ESP_OK;
}
