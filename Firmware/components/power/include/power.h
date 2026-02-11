#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#define POWER_5V_EN_PIN    23
#define POWER_12V_EN_PIN   32
#define POWER_12V_DIS_PIN  33
#define POWER_CAN_EN_PIN   16
#define POWER_5V_OK_PIN    35
#define POWER_12V_OK_PIN   34
#define POWER_BATT_ADC_PIN 36

esp_err_t power_init(void);
esp_err_t power_5v_enable(bool enable);
esp_err_t power_12v_enable(bool enable);
bool power_5v_ok(void);
int power_read_battery_mv(void);
esp_err_t power_can_enable(bool enable);
