#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/i2c_master.h"

#include "led.h"
#include "power.h"
#include "rs485.h"
#include "ping360.h"
#include "ms5837.h"
#include "wifi_ap.h"
#include "mdns_service.h"
#include "web_server.h"

/* Set to 0 to disable periodic heap/task diagnostics */
#define ENABLE_DIAG 0

#define NVS_CFG_NAMESPACE  "sonar_cfg"
#define NVS_CFG_KEY        "cfg"
#define NVS_CFG_VERSION    1
#define BOOTLOOP_NAMESPACE "bootloop"
#define BOOTLOOP_THRESHOLD 3

static const char *TAG = "main";

/* Calculate speed of sound in water (m/s).
 * Freshwater: Marczak (1997) simplified.
 * Saltwater:  Mackenzie (1981) at 35 PSU. */
static uint16_t calculate_speed_of_sound(float temp_c, float depth_m,
                                         bool saltwater)
{
    float c;
    if (saltwater) {
        /* Mackenzie (1981), S=35 PSU */
        c = 1448.96f + 4.591f * temp_c
            - 0.05304f * temp_c * temp_c
            + 0.0002374f * temp_c * temp_c * temp_c
            + 0.016f * depth_m;
    } else {
        /* Marczak (1997) simplified */
        c = 1402.7f + 5.0f * temp_c
            - 0.055f * temp_c * temp_c
            + 0.0003f * temp_c * temp_c * temp_c;
    }
    return (uint16_t)(c + 0.5f);
}

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

    /* Bootloop detection: if we crash 3 times before reaching stable state,
     * wipe saved settings on the next boot */
    {
        nvs_handle_t nvs;
        if (nvs_open(BOOTLOOP_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
            uint8_t boot_count = 0;
            nvs_get_u8(nvs, "count", &boot_count);
            if (boot_count >= BOOTLOOP_THRESHOLD) {
                ESP_LOGW(TAG, "Bootloop detected (%u crashes) — resetting settings",
                         boot_count);
                nvs_erase_all(nvs);
                nvs_commit(nvs);
                /* Erase saved config and depth calibration */
                nvs_handle_t cfg_nvs;
                if (nvs_open(NVS_CFG_NAMESPACE, NVS_READWRITE, &cfg_nvs) == ESP_OK) {
                    nvs_erase_all(cfg_nvs);
                    nvs_commit(cfg_nvs);
                    nvs_close(cfg_nvs);
                }
                nvs_handle_t cal_nvs;
                if (nvs_open("depth_cal", NVS_READWRITE, &cal_nvs) == ESP_OK) {
                    nvs_erase_all(cal_nvs);
                    nvs_commit(cal_nvs);
                    nvs_close(cal_nvs);
                }
            } else {
                boot_count++;
                nvs_set_u8(nvs, "count", boot_count);
                nvs_commit(nvs);
            }
            nvs_close(nvs);
        }
    }

    /* Load saved atmospheric pressure calibration from NVS */
    float atmo_pressure_mbar = 1013.25f;
    {
        nvs_handle_t nvs;
        if (nvs_open("depth_cal", NVS_READONLY, &nvs) == ESP_OK) {
            uint32_t raw = 0;
            if (nvs_get_u32(nvs, "atmo_mbar", &raw) == ESP_OK) {
                memcpy(&atmo_pressure_mbar, &raw, sizeof(float));
                ESP_LOGI(TAG, "Loaded atmo pressure from NVS: %.1f mbar",
                         atmo_pressure_mbar);
            }
            nvs_close(nvs);
        }
    }

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

    ms5837_t depth_sensor = {0};
    bool depth_ok = false;
    ret = ms5837_init(i2c_bus, &depth_sensor);
    if (ESP_OK == ret) {
        depth_ok = true;
        ms5837_set_atmo_pressure(&depth_sensor, atmo_pressure_mbar);
        ESP_LOGI(TAG, "MS5837 depth sensor initialized");
    } else {
        ESP_LOGW(TAG, "MS5837 init failed: %s (continuing without depth)",
                 esp_err_to_name(ret));
    }

    /* 10-12. RS-485 + Ping360 */
    ESP_ERROR_CHECK(rs485_init());
    ESP_ERROR_CHECK(ping360_init());

    /* Restore saved sonar config from NVS (if any) */
    {
        nvs_handle_t nvs;
        if (nvs_open(NVS_CFG_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
            uint8_t blob[1 + sizeof(ping360_config_t)];
            size_t len = sizeof(blob);
            if (nvs_get_blob(nvs, NVS_CFG_KEY, blob, &len) == ESP_OK &&
                len == sizeof(blob) && blob[0] == NVS_CFG_VERSION) {
                ping360_config_t cfg;
                memcpy(&cfg, &blob[1], sizeof(cfg));
                ping360_set_config(&cfg);
                ESP_LOGI(TAG, "Restored sonar config from NVS");
            }
            nvs_close(nvs);
        }
    }

    bool sonar_found = ping360_probe(2000);
    if (sonar_found) {
        led_set(LED_ID_2, true);
        ESP_LOGI(TAG, "Ping360 sonar detected");
    } else {
        ESP_LOGW(TAG, "Ping360 not detected (will retry during scan)");
    }

    /* 13-15. WiFi + mDNS + Web server */
    ESP_ERROR_CHECK(wifi_ap_init());
    ESP_ERROR_CHECK(mdns_service_init());
    ESP_ERROR_CHECK(web_server_init());

    /* Mark boot as stable — reset bootloop counter */
    {
        nvs_handle_t nvs;
        if (nvs_open(BOOTLOOP_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
            nvs_set_u8(nvs, "count", 0);
            nvs_commit(nvs);
            nvs_close(nvs);
        }
    }

    /* 15-16. Start sonar scan */
    ping360_register_data_callback(sonar_data_callback, NULL);
    ESP_ERROR_CHECK(ping360_start_scan());

    /* 17. All systems go */
    led_set(LED_ID_3, true);
    ESP_LOGI(TAG, "SonarMK2 ready");

    /* 18. Sensor poll loop (1 Hz) */
#if ENABLE_DIAG
    int diag_counter = 0;
#endif
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));

#if ENABLE_DIAG
        /* Periodic diagnostics (every 10s) */
        if (++diag_counter >= 10) {
            diag_counter = 0;

            size_t free_heap = esp_get_free_heap_size();
            size_t min_free = esp_get_minimum_free_heap_size();
            size_t largest_blk = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
            ESP_LOGI(TAG, "Heap: free=%u min_ever=%u largest_block=%u",
                     free_heap, min_free, largest_blk);

            UBaseType_t num_tasks = uxTaskGetNumberOfTasks();
            TaskStatus_t *tasks = malloc(num_tasks * sizeof(TaskStatus_t));
            if (tasks) {
                uint32_t total_runtime;
                UBaseType_t n = uxTaskGetSystemState(tasks, num_tasks,
                                                     &total_runtime);
                ESP_LOGI(TAG, "Tasks (%u):", n);
                for (UBaseType_t i = 0; i < n; i++) {
                    float cpu_pct = 0;
                    if (total_runtime > 0) {
                        cpu_pct = (tasks[i].ulRunTimeCounter * 100.0f)
                                  / total_runtime;
                    }
                    const char *st;
                    switch (tasks[i].eCurrentState) {
                        case eRunning:   st = "RUN"; break;
                        case eReady:     st = "RDY"; break;
                        case eBlocked:   st = "BLK"; break;
                        case eSuspended: st = "SUS"; break;
                        case eDeleted:   st = "DEL"; break;
                        default:         st = "???"; break;
                    }
                    ESP_LOGI(TAG, "  %-16s %s prio=%-2u stk_free=%-5u cpu=%.1f%%",
                             tasks[i].pcTaskName, st,
                             tasks[i].uxCurrentPriority,
                             tasks[i].usStackHighWaterMark,
                             cpu_pct);
                }
                free(tasks);
            }
        }
#endif

        float depth = 0, temp = 0, pressure = 0;
        if (depth_ok) {
            ret = ms5837_read(&depth_sensor);
            if (ret == ESP_OK) {
                depth = ms5837_get_depth(&depth_sensor);
                temp = ms5837_get_temperature(&depth_sensor);
                pressure = ms5837_get_pressure(&depth_sensor);
            }
        }

        /* Zero depth calibration: use current pressure as atmospheric ref */
        if (depth_ok && web_server_check_zero_depth()) {
            atmo_pressure_mbar = pressure;
            ms5837_set_atmo_pressure(&depth_sensor, atmo_pressure_mbar);
            depth = ms5837_get_depth(&depth_sensor);
            ESP_LOGI(TAG, "Depth zeroed: atmo=%.1f mbar", atmo_pressure_mbar);

            nvs_handle_t nvs;
            if (nvs_open("depth_cal", NVS_READWRITE, &nvs) == ESP_OK) {
                uint32_t raw;
                memcpy(&raw, &atmo_pressure_mbar, sizeof(float));
                nvs_set_u32(nvs, "atmo_mbar", raw);
                nvs_commit(nvs);
                nvs_close(nvs);
            }
        }

        /* Save sonar config to NVS when changed from web UI */
        if (web_server_check_config_changed()) {
            ping360_config_t save_cfg;
            ping360_get_config(&save_cfg);
            uint8_t blob[1 + sizeof(ping360_config_t)];
            blob[0] = NVS_CFG_VERSION;
            memcpy(&blob[1], &save_cfg, sizeof(save_cfg));
            nvs_handle_t nvs;
            if (nvs_open(NVS_CFG_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
                nvs_set_blob(nvs, NVS_CFG_KEY, blob, sizeof(blob));
                nvs_commit(nvs);
                nvs_close(nvs);
                ESP_LOGI(TAG, "Sonar config saved to NVS");
            }
        }

        /* Reset all settings to factory defaults */
        if (web_server_check_reset_settings()) {
            ESP_LOGI(TAG, "Resetting all settings to defaults");
            /* Erase saved config */
            nvs_handle_t nvs;
            if (nvs_open(NVS_CFG_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
                nvs_erase_all(nvs);
                nvs_commit(nvs);
                nvs_close(nvs);
            }
            /* Erase depth calibration */
            if (nvs_open("depth_cal", NVS_READWRITE, &nvs) == ESP_OK) {
                nvs_erase_all(nvs);
                nvs_commit(nvs);
                nvs_close(nvs);
            }
            /* Reset runtime state */
            ping360_reset_config();
            atmo_pressure_mbar = 1013.25f;
            ms5837_set_atmo_pressure(&depth_sensor, atmo_pressure_mbar);
            web_server_broadcast_config();
        }

        /* Update speed of sound from current temperature + water type */
        ping360_config_t cfg;
        ping360_get_config(&cfg);
        uint16_t new_sos = calculate_speed_of_sound(temp, depth, cfg.saltwater);
        if (new_sos != cfg.speed_of_sound) {
            cfg.speed_of_sound = new_sos;
            ping360_set_config(&cfg);
        }

        int batt_mv = power_read_battery_mv();
        float scan_rate = ping360_get_scan_rate();

        web_server_broadcast_status(depth, temp, pressure, batt_mv,
                                    scan_rate, sonar_found);
    }
}
