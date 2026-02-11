#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "driver/i2c_master.h"

#include "led.h"
#include "power.h"
#include "rs485.h"
#include "ping360.h"
#include "ms5837.h"
#include "wifi_ap.h"
#include "web_server.h"

static const char *TAG = "main";

static void sonar_data_callback(uint16_t angle, const uint8_t *data,
                                uint16_t num_samples, void *user_ctx)
{
    web_server_broadcast_sonar(angle, data, num_samples);
}

void app_main(void)
{
    /* 1. LEDs */
    ESP_ERROR_CHECK(led_init());
    led_set(LED_ID_0, true);
    ESP_LOGI(TAG, "Booting...");

    /* 2-4. Power management */
    ESP_ERROR_CHECK(power_init());
    ESP_ERROR_CHECK(power_5v_enable(true));
    vTaskDelay(pdMS_TO_TICKS(10));
    if (!power_5v_ok()) {
        ESP_LOGW(TAG, "5V rail not reporting OK");
    }
    ESP_ERROR_CHECK(power_12v_enable(true));
    vTaskDelay(pdMS_TO_TICKS(50));
    led_set(LED_ID_1, true);
    ESP_LOGI(TAG, "Power rails enabled");

    /* 6-7. NVS + network stack */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* 8-9. I2C + MS5837 */
    i2c_master_bus_config_t i2c_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .sda_io_num = MS5837_SDA_PIN,
        .scl_io_num = MS5837_SCL_PIN,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t i2c_bus = NULL;
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_cfg, &i2c_bus));

    ms5837_t depth_sensor;
    bool depth_ok = false;
    ret = ms5837_init(i2c_bus, &depth_sensor);
    if (ret == ESP_OK) {
        depth_ok = true;
        ESP_LOGI(TAG, "MS5837 depth sensor initialized");
    } else {
        ESP_LOGW(TAG, "MS5837 init failed: %s (continuing without depth)",
                 esp_err_to_name(ret));
    }

    /* 10-12. RS-485 + Ping360 */
    ESP_ERROR_CHECK(rs485_init());
    ESP_ERROR_CHECK(ping360_init());

    bool sonar_found = ping360_probe(2000);
    if (sonar_found) {
        led_set(LED_ID_2, true);
        ESP_LOGI(TAG, "Ping360 sonar detected");
    } else {
        ESP_LOGW(TAG, "Ping360 not detected (will retry during scan)");
    }

    /* 13-14. WiFi + Web server */
    ESP_ERROR_CHECK(wifi_ap_init());
    ESP_ERROR_CHECK(web_server_init());

    /* 15-16. Start sonar scan */
    ping360_register_data_callback(sonar_data_callback, NULL);
    ESP_ERROR_CHECK(ping360_start_scan());

    /* 17. All systems go */
    led_set(LED_ID_3, true);
    ESP_LOGI(TAG, "SonarMK2 ready");

    /* 18. Sensor poll loop (1 Hz) */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        float depth = 0, temp = 0, pressure = 0;
        if (depth_ok) {
            ret = ms5837_read(&depth_sensor);
            if (ret == ESP_OK) {
                depth = ms5837_get_depth(&depth_sensor);
                temp = ms5837_get_temperature(&depth_sensor);
                pressure = ms5837_get_pressure(&depth_sensor);
            }
        }

        int batt_mv = power_read_battery_mv();
        float scan_rate = ping360_get_scan_rate();

        web_server_broadcast_status(depth, temp, pressure, batt_mv,
                                    scan_rate, sonar_found);
    }
}
