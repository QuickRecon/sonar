#include "ms5837.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "ms5837";

#define MS5837_CMD_RESET       0x1E
#define MS5837_CMD_PROM_READ   0xA0
#define MS5837_CMD_CONVERT_D1  0x48  /* OSR 4096 */
#define MS5837_CMD_CONVERT_D2  0x58  /* OSR 4096 */
#define MS5837_CMD_ADC_READ    0x00

static esp_err_t ms5837_write_cmd(ms5837_t *handle, uint8_t cmd)
{
    return i2c_master_transmit(handle->dev, &cmd, 1, 100);
}

static esp_err_t ms5837_read_adc(ms5837_t *handle, uint32_t *value)
{
    uint8_t cmd = MS5837_CMD_ADC_READ;
    uint8_t data[3] = {0};

    esp_err_t ret = i2c_master_transmit_receive(handle->dev, &cmd, 1,
                                                 data, 3, 100);
    if (ret != ESP_OK) return ret;

    *value = ((uint32_t)data[0] << 16) | ((uint32_t)data[1] << 8) | data[2];
    return ESP_OK;
}

esp_err_t ms5837_init(i2c_master_bus_handle_t bus, ms5837_t *handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;

    memset(handle, 0, sizeof(*handle));
    handle->fluid_density = 1029.0f;
    handle->atmo_pressure_mbar = 1013.25f;

    /* Add device to bus */
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = MS5837_I2C_ADDR,
        .scl_speed_hz = 400000,
    };
    esp_err_t ret = i2c_master_bus_add_device(bus, &dev_cfg, &handle->dev);
    if (ret != ESP_OK) return ret;

    /* Reset sensor */
    ret = ms5837_write_cmd(handle, MS5837_CMD_RESET);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Reset failed");
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(15));

    /* Read PROM calibration data (C1-C6 at addresses 0xA2-0xAC) */
    for (int i = 0; i < 7; i++) {
        uint8_t cmd = MS5837_CMD_PROM_READ + (i * 2);
        uint8_t data[2] = {0};
        ret = i2c_master_transmit_receive(handle->dev, &cmd, 1, data, 2, 100);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "PROM read failed at index %d", i);
            return ret;
        }
        handle->cal[i] = ((uint16_t)data[0] << 8) | data[1];
    }

    ESP_LOGI(TAG, "MS5837 initialized (cal: %u %u %u %u %u %u)",
             handle->cal[1], handle->cal[2], handle->cal[3],
             handle->cal[4], handle->cal[5], handle->cal[6]);
    return ESP_OK;
}

esp_err_t ms5837_read(ms5837_t *handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;

    /* Convert D1 (pressure) */
    esp_err_t ret = ms5837_write_cmd(handle, MS5837_CMD_CONVERT_D1);
    if (ret != ESP_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(12));

    uint32_t d1 = 0;
    ret = ms5837_read_adc(handle, &d1);
    if (ret != ESP_OK) return ret;

    /* Convert D2 (temperature) */
    ret = ms5837_write_cmd(handle, MS5837_CMD_CONVERT_D2);
    if (ret != ESP_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(12));

    uint32_t d2 = 0;
    ret = ms5837_read_adc(handle, &d2);
    if (ret != ESP_OK) return ret;

    /* First-order compensation (MS5837-30BA datasheet) */
    int32_t dT = (int32_t)d2 - ((int32_t)handle->cal[5] << 8);
    int32_t temp = 2000 + (int64_t)dT * handle->cal[6] / 8388608;

    int64_t off  = (int64_t)handle->cal[2] * 65536 +
                   ((int64_t)handle->cal[4] * dT) / 128;
    int64_t sens = (int64_t)handle->cal[1] * 32768 +
                   ((int64_t)handle->cal[3] * dT) / 256;

    /* Second-order compensation */
    int64_t ti = 0, offi = 0, sensi = 0;
    if (temp < 2000) {
        ti    = 3 * ((int64_t)dT * dT) / 8589934592LL;
        offi  = 3 * ((int64_t)(temp - 2000) * (temp - 2000)) / 2;
        sensi = 5 * ((int64_t)(temp - 2000) * (temp - 2000)) / 8;
        if (temp < -1500) {
            offi  += 7 * ((int64_t)(temp + 1500) * (temp + 1500));
            sensi += 4 * ((int64_t)(temp + 1500) * (temp + 1500));
        }
    } else {
        ti    = 2 * ((int64_t)dT * dT) / 137438953472LL;
        offi  = (int64_t)(temp - 2000) * (temp - 2000) / 16;
        sensi = 0;
    }

    off  -= offi;
    sens -= sensi;

    int32_t pressure = ((int64_t)d1 * sens / 2097152 - off) / 8192;
    temp -= ti;

    handle->pressure_mbar = pressure / 10.0f;
    handle->temperature_c = temp / 100.0f;
    handle->depth_m = (handle->pressure_mbar - handle->atmo_pressure_mbar) /
                      (handle->fluid_density * 9.80665f / 1000.0f);

    return ESP_OK;
}

float ms5837_get_depth(const ms5837_t *handle)
{
    return handle ? handle->depth_m : 0.0f;
}

float ms5837_get_temperature(const ms5837_t *handle)
{
    return handle ? handle->temperature_c : 0.0f;
}

float ms5837_get_pressure(const ms5837_t *handle)
{
    return handle ? handle->pressure_mbar : 0.0f;
}

void ms5837_set_fluid_density(ms5837_t *handle, float density)
{
    if (handle) handle->fluid_density = density;
}

void ms5837_set_atmo_pressure(ms5837_t *handle, float mbar)
{
    if (handle) handle->atmo_pressure_mbar = mbar;
}
