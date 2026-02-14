#include "power.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/gpio_types.h"

static const char *TAG = "power";

static adc_oneshot_unit_handle_t adc_handle;
static adc_cali_handle_t adc_cali_handle;

esp_err_t power_init(void)
{
    /* Output GPIOs */
    gpio_config_t pwr_5v_cfg = {
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
        .pin_bit_mask = (1ULL << POWER_5V_EN_PIN),
    };
    ESP_RETURN_ON_ERROR(gpio_config(&pwr_5v_cfg), TAG, "Error setting up 5v rail");


    gpio_config_t pwr_12v_cfg = {
        .mode = GPIO_MODE_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
        .pin_bit_mask = (1ULL << POWER_12V_SMPS_EN_PIN) |
                        (1ULL << POWER_12V_DIS_PIN),
    };
    ESP_RETURN_ON_ERROR(gpio_config(&pwr_12v_cfg), TAG, "Error setting up 12v rail");

    /* Start with rails disabled */
    gpio_set_level(POWER_5V_EN_PIN, 0);
    gpio_set_level(POWER_12V_SMPS_EN_PIN, 0);
    gpio_set_level(POWER_12V_DIS_PIN, 1);

    /* Input GPIOs */
    gpio_config_t in_cfg = {
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
        .pin_bit_mask = (1ULL << POWER_5V_OK_PIN) |
                        (1ULL << POWER_12V_OK_PIN),
    };
    ESP_RETURN_ON_ERROR(gpio_config(&in_cfg), TAG, "Error setting up pwr status inputs");

    /* ADC for battery voltage on GPIO36 (ADC1_CH0) */
    adc_oneshot_unit_init_cfg_t adc_cfg = {
        .unit_id = ADC_UNIT_1,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&adc_cfg, &adc_handle), TAG, "Error setting up adc");

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(adc_handle, ADC_CHANNEL_0, &chan_cfg), TAG, "Error setting up adc channel");

    /* ADC calibration */
    adc_cali_line_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    esp_err_t cal_ret = adc_cali_create_scheme_line_fitting(&cali_cfg, &adc_cali_handle);
    if (cal_ret != ESP_OK) {
        ESP_LOGW(TAG, "ADC calibration failed, readings may be inaccurate");
        adc_cali_handle = NULL;
    }

    ESP_LOGI(TAG, "Power management initialized");
    return ESP_OK;
}

esp_err_t power_5v_enable(bool enable)
{
    ESP_LOGI(TAG, "5V rail %s", enable ? "ON" : "OFF");

    esp_err_t err = ESP_OK;
    if(enable){
        err += gpio_set_level(POWER_5V_EN_PIN, 1);
    } else {
        err += gpio_set_level(POWER_5V_EN_PIN, 0);
    }
    return err;
}

esp_err_t power_12v_enable(bool enable)
{
    ESP_LOGI(TAG, "12V rail %s", enable ? "ON" : "OFF");
    esp_err_t err = ESP_OK;
    if (enable) {
        err += gpio_set_level(POWER_12V_SMPS_EN_PIN, 1);
        err += gpio_set_level(POWER_12V_DIS_PIN, 0);
    } else {
        err += gpio_set_level(POWER_12V_DIS_PIN, 1);
        err += gpio_set_level(POWER_12V_SMPS_EN_PIN, 0);
    }
    return err;
}

bool power_5v_ok(void)
{
    return 1 == gpio_get_level(POWER_5V_OK_PIN);
}

int32_t power_read_battery_mv(void)
{
    int32_t raw = 0;
    esp_err_t ret = adc_oneshot_read(adc_handle, ADC_CHANNEL_0, (int*)&raw);
    if (ret != ESP_OK){
        raw = -1;
        ESP_LOGW(TAG, "Failed to read battery voltage: %s", esp_err_to_name(ret));
    }

    int32_t voltage_mv = 0;
    if (adc_cali_handle != NULL) {
        adc_cali_raw_to_voltage(adc_cali_handle, raw, (int*)&voltage_mv);
    } else {
        /* Rough conversion without calibration: 12-bit, 3.3V ref, 12dB atten */
        voltage_mv = (raw * 3300) / 4095;
    }

    /* Multiply by 3 for voltage divider ratio */
    return voltage_mv * 3;
}
