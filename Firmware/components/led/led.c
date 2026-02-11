#include "led.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "led";

static const gpio_num_t led_pins[LED_COUNT] = {
    LED0_PIN, LED1_PIN, LED2_PIN, LED3_PIN
};

static bool led_state[LED_COUNT];

esp_err_t led_init(void)
{
    gpio_config_t cfg = {
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
        .pin_bit_mask = 0,
    };

    for (int i = 0; i < LED_COUNT; i++) {
        cfg.pin_bit_mask |= (1ULL << led_pins[i]);
    }

    esp_err_t ret = gpio_config(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GPIO config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    for (int i = 0; i < LED_COUNT; i++) {
        gpio_set_level(led_pins[i], 0);
        led_state[i] = false;
    }

    ESP_LOGI(TAG, "LEDs initialized");
    return ESP_OK;
}

esp_err_t led_set(led_id_t id, bool on)
{
    if (id >= LED_COUNT) return ESP_ERR_INVALID_ARG;
    led_state[id] = on;
    return gpio_set_level(led_pins[id], on ? 1 : 0);
}

esp_err_t led_toggle(led_id_t id)
{
    if (id >= LED_COUNT) return ESP_ERR_INVALID_ARG;
    led_state[id] = !led_state[id];
    return gpio_set_level(led_pins[id], led_state[id] ? 1 : 0);
}

esp_err_t led_set_all(uint8_t mask)
{
    for (int i = 0; i < LED_COUNT; i++) {
        bool on = (mask >> i) & 1;
        led_state[i] = on;
        gpio_set_level(led_pins[i], on ? 1 : 0);
    }
    return ESP_OK;
}
