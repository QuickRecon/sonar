#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

esp_err_t wifi_ap_init(void);
esp_err_t wifi_ap_get_ip(char *buf, size_t len);
int wifi_ap_get_station_count(void);
