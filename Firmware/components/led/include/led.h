#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#define LED0_PIN  15
#define LED1_PIN  13
#define LED2_PIN  14
#define LED3_PIN  12
#define LED_COUNT 4

typedef enum {
    LED_ID_0 = 0,
    LED_ID_1,
    LED_ID_2,
    LED_ID_3,
} led_id_t;

esp_err_t led_init(void);
esp_err_t led_set(led_id_t id, bool on);
esp_err_t led_toggle(led_id_t id);
esp_err_t led_set_all(uint8_t mask);
