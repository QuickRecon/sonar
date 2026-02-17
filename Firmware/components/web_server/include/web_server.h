#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

esp_err_t web_server_init(void);
esp_err_t web_server_broadcast_sonar(uint16_t angle, const uint8_t *data,
                                     uint16_t num_samples);
esp_err_t web_server_broadcast_status(float depth_m, float temp_c,
                                      float pressure_mbar, int batt_mv,
                                      float scan_rate, bool sonar_connected);
bool web_server_check_zero_depth(void);
bool web_server_check_config_changed(void);
bool web_server_check_reset_settings(void);
void web_server_broadcast_config(void);
