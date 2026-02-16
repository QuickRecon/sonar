#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"
#include <stdint.h>

#define MS5837_I2C_ADDR  0x76
#define MS5837_SDA_PIN   21
#define MS5837_SCL_PIN   22

typedef struct {
    i2c_master_dev_handle_t dev;
    uint16_t cal[8];
    float pressure_mbar;
    float temperature_c;
    float depth_m;
    float fluid_density;
    float atmo_pressure_mbar;
} ms5837_t;

esp_err_t ms5837_init(i2c_master_bus_handle_t bus, ms5837_t *handle);
esp_err_t ms5837_read(ms5837_t *handle);
float ms5837_get_depth(const ms5837_t *handle);
float ms5837_get_temperature(const ms5837_t *handle);
float ms5837_get_pressure(const ms5837_t *handle);
void ms5837_set_fluid_density(ms5837_t *handle, float density);
void ms5837_set_atmo_pressure(ms5837_t *handle, float mbar);
