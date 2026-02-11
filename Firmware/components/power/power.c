#include "power.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "power";

static adc_oneshot_unit_handle_t adc_handle;
static adc_cali_handle_t adc_cali_handle;

esp_err_t power_init(void)
{
    /* Output GPIOs */
    gpio_config_t out_cfg = {
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
        .pin_bit_mask = (1ULL << POWER_5V_EN_PIN) |
                        (1ULL << POWER_12V_EN_PIN) |
                        (1ULL << POWER_12V_DIS_PIN) |
                        (1ULL << POWER_CAN_EN_PIN),
    };
    esp_err_t ret = gpio_config(&out_cfg);
    if (ret != ESP_OK) return ret;

    /* Start with rails disabled */
    gpio_set_level(POWER_5V_EN_PIN, 0);
    gpio_set_level(POWER_12V_EN_PIN, 0);
    gpio_set_level(POWER_12V_DIS_PIN, 0);
    gpio_set_level(POWER_CAN_EN_PIN, 0);

    /* Input GPIOs */
    gpio_config_t in_cfg = {
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
        .pin_bit_mask = (1ULL << POWER_5V_OK_PIN) |
                        (1ULL << POWER_12V_OK_PIN),
    };
    ret = gpio_config(&in_cfg);
    if (ret != ESP_OK) return ret;

    /* ADC for battery voltage on GPIO36 (ADC1_CH0) */
    adc_oneshot_unit_init_cfg_t adc_cfg = {
        .unit_id = ADC_UNIT_1,
    };
    ret = adc_oneshot_new_unit(&adc_cfg, &adc_handle);
    if (ret != ESP_OK) return ret;

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ret = adc_oneshot_config_channel(adc_handle, ADC_CHANNEL_0, &chan_cfg);
    if (ret != ESP_OK) return ret;

    /* ADC calibration */
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_1,
        .chan = ADC_CHANNEL_0,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ret = adc_cali_create_scheme_curve_fitting(&cali_cfg, &adc_cali_handle);
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ret = adc_cali_create_scheme_line_fitting(&cali_cfg, &adc_cali_handle);
#endif
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ADC calibration failed, readings may be inaccurate");
        adc_cali_handle = NULL;
    }

    ESP_LOGI(TAG, "Power management initialized");
    return ESP_OK;
}

esp_err_t power_5v_enable(bool enable)
{
    ESP_LOGI(TAG, "5V rail %s", enable ? "ON" : "OFF");
    return gpio_set_level(POWER_5V_EN_PIN, enable ? 1 : 0);
}

esp_err_t power_12v_enable(bool enable)
{
    ESP_LOGI(TAG, "12V rail %s", enable ? "ON" : "OFF");
    if (enable) {
        gpio_set_level(POWER_12V_DIS_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(1));
        return gpio_set_level(POWER_12V_EN_PIN, 1);
    } else {
        gpio_set_level(POWER_12V_EN_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(1));
        return gpio_set_level(POWER_12V_DIS_PIN, 1);
    }
}

bool power_5v_ok(void)
{
    return gpio_get_level(POWER_5V_OK_PIN) == 1;
}

int power_read_battery_mv(void)
{
    int raw = 0;
    esp_err_t ret = adc_oneshot_read(adc_handle, ADC_CHANNEL_0, &raw);
    if (ret != ESP_OK) return -1;

    int voltage_mv = 0;
    if (adc_cali_handle) {
        adc_cali_raw_to_voltage(adc_cali_handle, raw, &voltage_mv);
    } else {
        /* Rough conversion without calibration: 12-bit, 3.3V ref, 12dB atten */
        voltage_mv = (raw * 3300) / 4095;
    }

    /* Multiply by 3 for voltage divider ratio */
    return voltage_mv * 3;
}

esp_err_t power_can_enable(bool enable)
{
    ESP_LOGI(TAG, "CAN transceiver %s", enable ? "ON" : "OFF");
    return gpio_set_level(POWER_CAN_EN_PIN, enable ? 1 : 0);
}
