#include "ping360.h"
#include "ping_protocol.h"
#include "rs485.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "ping360";

static ping360_config_t s_config;
static SemaphoreHandle_t s_mutex;
static TaskHandle_t s_task;
static volatile bool s_stop;
static ping360_data_cb_t s_data_cb;
static void *s_data_cb_ctx;
static volatile float s_scan_rate;
static volatile bool s_connected;

#define TICK_DURATION_NS   25e-9f   /* 25 ns per sample_period tick */
#define MIN_SAMPLE_PERIOD  80       /* firmware minimum */
#define MAX_NUM_SAMPLES    1200     /* firmware maximum */
#define MIN_TX_DURATION    5        /* microseconds */
#define MAX_TX_DURATION    500      /* microseconds */

/* Recalculate sample_period and transmit_duration from range, num_samples,
 * and speed_of_sound.  Matches the ping-viewer formulas.
 * Must be called while s_mutex is held (or before any task is running). */
static void recalculate_params(void)
{
    float range_m = s_config.range_mm / 1000.0f;
    float speed   = (float)s_config.speed_of_sound;
    uint16_t ns   = s_config.num_samples;

    /* sample_period = round-trip time / num_samples, in 25 ns ticks */
    uint16_t sp = (uint16_t)(2.0f * range_m / (ns * speed * TICK_DURATION_NS) + 0.5f);

    /* If below firmware minimum, reduce num_samples to fit */
    if (sp < MIN_SAMPLE_PERIOD) {
        ns = (uint16_t)(2.0f * range_m / (MIN_SAMPLE_PERIOD * speed * TICK_DURATION_NS));
        if (ns < 1) ns = 1;
        sp = MIN_SAMPLE_PERIOD;
        s_config.num_samples = ns;
    }
    s_config.sample_period = sp;

    /* transmit_duration (microseconds) */
    int td = (int)(8000.0f * range_m / speed + 0.5f);
    float sp_us = sp * TICK_DURATION_NS * 1e6f;
    int td_min = (int)(2.5f * sp_us + 0.5f);
    if (td < td_min) td = td_min;
    if (td < MIN_TX_DURATION) td = MIN_TX_DURATION;
    if (td > MAX_TX_DURATION) td = MAX_TX_DURATION;
    s_config.transmit_duration = (uint16_t)td;
}

static void ping360_default_config(void)
{
    s_config.mode = 1;
    s_config.gain = 1;
    s_config.start_angle = 0;
    s_config.end_angle = 399;
    s_config.num_samples = MAX_NUM_SAMPLES;
    s_config.transmit_frequency = 740;
    s_config.range_mm = 5000;
    s_config.speed_of_sound = 1500;
    s_config.saltwater = true;
    recalculate_params(); /* sets sample_period + transmit_duration */
}

static int ping360_transact(const uint8_t *tx, size_t tx_len,
                            ping_parser_t *parser, uint32_t timeout_ms)
{
    rs485_flush_rx();
    int sent = rs485_send(tx, tx_len);
    if (sent < 0) return -1;

    ping_parser_init(parser);
    uint8_t byte;
    TickType_t start = xTaskGetTickCount();
    TickType_t deadline = start + pdMS_TO_TICKS(timeout_ms);

    while (xTaskGetTickCount() < deadline) {
        TickType_t remaining = deadline - xTaskGetTickCount();
        uint32_t wait_ms = remaining * portTICK_PERIOD_MS;
        if (wait_ms == 0) wait_ms = 1;

        int n = rs485_recv(&byte, 1, wait_ms);
        if (n <= 0) continue;

        int result = ping_parser_feed(parser, byte);
        if (result == 1) return 0;
        if (result < 0) ping_parser_init(parser);
    }
    return -1;
}

static void sonar_task(void *arg)
{
    uint8_t tx_buf[PING_MAX_FRAME_LEN];
    ping_parser_t parser;
    ping_device_data_t dev_data;
    uint32_t angle_count = 0;
    TickType_t rate_start = xTaskGetTickCount();
    int direction = 1; /* 1 = forward, -1 = reverse */

    ESP_LOGI(TAG, "Scan task started");

    while (!s_stop) {
        ping360_config_t cfg;
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        memcpy(&cfg, &s_config, sizeof(cfg));
        xSemaphoreGive(s_mutex);

        uint16_t start = cfg.start_angle;
        uint16_t end = cfg.end_angle;
        bool full_scan = (start == 0 && end == 399);

        /* Calculate number of angles in this sector */
        uint16_t sector_size;
        if (full_scan) {
            sector_size = 400;
        } else {
            sector_size = (end - start + 400) % 400 + 1;
        }

        for (int step = 0; step < sector_size && !s_stop; step++) {
            uint16_t angle;
            if (direction == 1) {
                angle = (start + step) % 400;
            } else {
                angle = (end + 400 - step) % 400;
            }

            ping_transducer_cmd_t cmd = {
                .mode = cfg.mode,
                .gain = cfg.gain,
                .angle = angle,
                .transmit_duration = cfg.transmit_duration,
                .sample_period = cfg.sample_period,
                .transmit_frequency = cfg.transmit_frequency,
                .num_samples = cfg.num_samples,
                .transmit = 1,
            };

            int frame_len = ping_build_transducer_cmd(tx_buf, sizeof(tx_buf), &cmd);
            if (frame_len < 0) {
                ESP_LOGE(TAG, "Failed to build transducer cmd");
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }

            int ret = ping360_transact(tx_buf, frame_len, &parser, 4000);
            if (ret == 0 && ping_parse_device_data(&parser, &dev_data) == 0) {
                s_connected = true;
                if (s_data_cb) {
                    s_data_cb(dev_data.angle, dev_data.data,
                              dev_data.num_samples, s_data_cb_ctx);
                }
                angle_count++;
            } else {
                s_connected = false;
                ESP_LOGW(TAG, "No response at angle %u", angle);
                vTaskDelay(pdMS_TO_TICKS(50));
            }

            /* Update scan rate every second */
            TickType_t now = xTaskGetTickCount();
            TickType_t elapsed = now - rate_start;
            if (elapsed >= pdMS_TO_TICKS(1000)) {
                s_scan_rate = (float)angle_count * 1000.0f /
                              (elapsed * portTICK_PERIOD_MS);
                angle_count = 0;
                rate_start = now;
            }
        }

        /* After completing a sweep: bounce for sectors, keep going for 360° */
        if (!full_scan) {
            direction = -direction;
        }
    }

    ESP_LOGI(TAG, "Scan task stopped");
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t ping360_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;

    ping360_default_config();
    s_task = NULL;
    s_stop = false;
    s_data_cb = NULL;
    s_data_cb_ctx = NULL;
    s_scan_rate = 0;
    s_connected = false;

    ESP_LOGI(TAG, "Ping360 driver initialized");
    return ESP_OK;
}

void ping360_register_data_callback(ping360_data_cb_t cb, void *user_ctx)
{
    s_data_cb = cb;
    s_data_cb_ctx = user_ctx;
}

esp_err_t ping360_start_scan(void)
{
    if (s_task) return ESP_ERR_INVALID_STATE;

    s_stop = false;
    BaseType_t ret = xTaskCreatePinnedToCore(sonar_task, "sonar_task",
                                              4096, NULL, 5, &s_task, 1);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create scan task");
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t ping360_stop_scan(void)
{
    if (!s_task) return ESP_OK;

    s_stop = true;
    /* Wait for task to finish */
    for (int i = 0; i < 100 && s_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (s_task) {
        ESP_LOGW(TAG, "Scan task did not stop in time");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t ping360_set_config(const ping360_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memcpy(&s_config, config, sizeof(s_config));
    recalculate_params();
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t ping360_get_config(ping360_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memcpy(config, &s_config, sizeof(*config));
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

bool ping360_probe(uint32_t timeout_ms)
{
    uint8_t tx_buf[32];
    int frame_len = ping_build_general_request(tx_buf, sizeof(tx_buf),
                                                PING_MSG_PROTOCOL_VERSION);
    if (frame_len < 0) return false;

    ping_parser_t parser;
    int ret = ping360_transact(tx_buf, frame_len, &parser, timeout_ms);
    if (ret == 0) {
        ESP_LOGI(TAG, "Ping360 detected (msg_id=%u)", parser.msg_id);
        s_connected = true;
        return true;
    }

    ESP_LOGW(TAG, "Ping360 not detected");
    s_connected = false;
    return false;
}

esp_err_t ping360_motor_off(void)
{
    uint8_t tx_buf[32];
    int frame_len = ping_build_motor_off(tx_buf, sizeof(tx_buf));
    if (frame_len < 0) return ESP_FAIL;

    rs485_flush_rx();
    int sent = rs485_send(tx_buf, frame_len);
    return (sent >= 0) ? ESP_OK : ESP_FAIL;
}

float ping360_get_scan_rate(void)
{
    return s_scan_rate;
}
