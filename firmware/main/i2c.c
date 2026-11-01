#include "esp_log.h"
#include "i2c.h"

static const char* TAG = "i2c";

static i2c_master_bus_handle_t s_bus = NULL;

void i2c_init(void)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port          = I2C_MASTER_PORT,
        .sda_io_num        = I2C_MASTER_SDA_IO,
        .scl_io_num        = I2C_MASTER_SCL_IO,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &s_bus));
    ESP_LOGI(TAG, "I2C master bus ready (SDA=%d SCL=%d %dkHz)",
             I2C_MASTER_SDA_IO, I2C_MASTER_SCL_IO, I2C_MASTER_FREQ_HZ / 1000);
}

i2c_master_bus_handle_t i2c_get_bus(void)
{
    return s_bus;
}
