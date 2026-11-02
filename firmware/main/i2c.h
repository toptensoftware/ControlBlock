#pragma once
#include "driver/i2c_master.h"

#define I2C_MASTER_SCL_IO    GPIO_NUM_9
#define I2C_MASTER_SDA_IO    GPIO_NUM_8
#define I2C_MASTER_PORT      I2C_NUM_0
#define I2C_MASTER_FREQ_HZ   400000
#define I2C_MASTER_TIMEOUT_MS 1000

// Initialise the I2C master bus.
void i2c_init(void);

// Returns the bus handle so other modules can add their own devices via
// i2c_master_bus_add_device().
i2c_master_bus_handle_t i2c_get_bus(void);
