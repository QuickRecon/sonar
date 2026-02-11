#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

#define CANBUS_TX_PIN  19
#define CANBUS_RX_PIN  18
#define CANBUS_BITRATE 250000

esp_err_t canbus_init(void);
esp_err_t canbus_start(void);
esp_err_t canbus_stop(void);
esp_err_t canbus_send(uint32_t id, const uint8_t *data, size_t len);
esp_err_t canbus_deinit(void);
