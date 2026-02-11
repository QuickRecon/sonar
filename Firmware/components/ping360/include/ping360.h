#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t  mode;
    uint8_t  gain;
    uint16_t start_angle;
    uint16_t end_angle;
    uint16_t num_samples;
    uint16_t transmit_frequency;
    uint16_t transmit_duration;
    uint16_t sample_period;
    uint32_t range_mm;
    uint16_t speed_of_sound;
} ping360_config_t;

typedef void (*ping360_data_cb_t)(uint16_t angle, const uint8_t *data,
                                  uint16_t num_samples, void *user_ctx);

esp_err_t ping360_init(void);
void ping360_register_data_callback(ping360_data_cb_t cb, void *user_ctx);
esp_err_t ping360_start_scan(void);
esp_err_t ping360_stop_scan(void);
esp_err_t ping360_set_config(const ping360_config_t *config);
esp_err_t ping360_get_config(ping360_config_t *config);
bool ping360_probe(uint32_t timeout_ms);
esp_err_t ping360_motor_off(void);
float ping360_get_scan_rate(void);
