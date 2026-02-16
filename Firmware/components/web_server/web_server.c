#include "web_server.h"
#include "ping360.h"
#include "power.h"
#include "esp_http_server.h"
#include "esp_https_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

/* Embedded web assets (linked into binary via EMBED_TXTFILES) */
extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[]   asm("_binary_index_html_end");
extern const char style_css_start[]  asm("_binary_style_css_start");
extern const char style_css_end[]    asm("_binary_style_css_end");
extern const char sonar_js_start[]   asm("_binary_sonar_js_start");
extern const char sonar_js_end[]     asm("_binary_sonar_js_end");

/* Embedded TLS certificates */
extern const uint8_t server_pem_start[] asm("_binary_server_pem_start");
extern const uint8_t server_pem_end[]   asm("_binary_server_pem_end");
extern const uint8_t server_key_start[] asm("_binary_server_key_start");
extern const uint8_t server_key_end[]   asm("_binary_server_key_end");

static const char *TAG = "web_server";

static httpd_handle_t s_server = NULL;       /* HTTPS server (port 443) */

#define MAX_WS_CLIENTS 4

static int s_ws_fds[MAX_WS_CLIENTS];
static int s_ws_count = 0;
static SemaphoreHandle_t s_ws_mutex;

/* Debug counters — reset each status broadcast */
static uint32_t s_ws_send_ok = 0;
static uint32_t s_ws_send_fail = 0;

/* Zero-depth calibration request flag */
static volatile bool s_zero_depth_requested = false;

/* Static buffer for sonar WS payload (no WS header — httpd handles framing).
 * Only accessed from sonar task (via web_server_broadcast_sonar). */
static uint8_t s_sonar_payload_buf[5 + 1200];

/* ---- WebSocket client management ---- */

/* Add client to list. Caller must hold s_ws_mutex. */
static void ws_add_client(int fd)
{
    if (s_ws_count >= MAX_WS_CLIENTS) {
        /* LRU purge: drop oldest */
        ESP_LOGW(TAG, "Max WS clients, dropping fd=%d", s_ws_fds[0]);
        httpd_sess_trigger_close(s_server, s_ws_fds[0]);
        memmove(&s_ws_fds[0], &s_ws_fds[1],
                (MAX_WS_CLIENTS - 1) * sizeof(int));
        s_ws_count--;
    }
    s_ws_fds[s_ws_count++] = fd;
    ESP_LOGI(TAG, "WS client connected (fd=%d, total=%d)", fd, s_ws_count);
}

/* Remove client from list. Caller must hold s_ws_mutex. */
static void ws_remove_client(int fd)
{
    for (int i = 0; i < s_ws_count; i++) {
        if (s_ws_fds[i] == fd) {
            memmove(&s_ws_fds[i], &s_ws_fds[i + 1],
                    (s_ws_count - i - 1) * sizeof(int));
            s_ws_count--;
            ESP_LOGI(TAG, "WS client disconnected (fd=%d, total=%d)",
                     fd, s_ws_count);
            return;
        }
    }
}

/* ---- Send current config to a single client ---- */

/* Caller must hold s_ws_mutex. */
static void send_config_to_client(int fd)
{
    ping360_config_t cfg;
    ping360_get_config(&cfg);

    char buf[300];
    int len = snprintf(buf, sizeof(buf),
        "{\"type\":\"config\",\"gain\":%u,\"start_angle\":%u,"
        "\"end_angle\":%u,\"num_samples\":%u,"
        "\"transmit_frequency\":%u,\"transmit_duration\":%u,"
        "\"sample_period\":%u,\"range_mm\":%lu,"
        "\"speed_of_sound\":%u,\"saltwater\":%s}",
        cfg.gain, cfg.start_angle, cfg.end_angle, cfg.num_samples,
        cfg.transmit_frequency, cfg.transmit_duration,
        cfg.sample_period, (unsigned long)cfg.range_mm,
        cfg.speed_of_sound, cfg.saltwater ? "true" : "false");

    httpd_ws_frame_t frame = {
        .final = true,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)buf,
        .len = len,
    };
    httpd_ws_send_frame_async(s_server, fd, &frame);
}

/* ---- WebSocket handler ---- */

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        int fd = httpd_req_to_sockfd(req);
        xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
        ws_add_client(fd);
        send_config_to_client(fd);
        xSemaphoreGive(s_ws_mutex);
        return ESP_OK;
    }

    httpd_ws_frame_t ws_frame = {0};
    ws_frame.type = HTTPD_WS_TYPE_TEXT;

    /* Get frame length */
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_frame, 0);
    if (ret != ESP_OK) return ret;
    if (ws_frame.len == 0) return ESP_OK;

    uint8_t *buf = malloc(ws_frame.len + 1);
    if (!buf) return ESP_ERR_NO_MEM;

    ws_frame.payload = buf;
    ret = httpd_ws_recv_frame(req, &ws_frame, ws_frame.len);
    if (ret != ESP_OK) {
        free(buf);
        return ret;
    }
    buf[ws_frame.len] = '\0';

    /* Parse JSON command (lightweight — no cJSON dependency) */
    char *json = (char *)buf;

    /* Handle reset commands */
    if (strstr(json, "\"reset_sonar\"")) {
        free(buf);
        ESP_LOGI(TAG, "Sonar reset requested - cycling 12V rail");
        power_12v_enable(false);
        vTaskDelay(pdMS_TO_TICKS(500));
        power_12v_enable(true);
        return ESP_OK;
    }

    if (strstr(json, "\"reset_controller\"")) {
        free(buf);
        ESP_LOGI(TAG, "Controller reset requested - rebooting");
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
        return ESP_OK;
    }

    if (strstr(json, "\"zero_depth\"")) {
        s_zero_depth_requested = true;
        ESP_LOGI(TAG, "Zero depth calibration requested");
        free(buf);
        return ESP_OK;
    }

    if (!strstr(json, "\"set_config\"")) {
        free(buf);
        return ESP_OK;
    }

    ping360_config_t cfg;
    ping360_get_config(&cfg);

    char *p;
    if ((p = strstr(json, "\"gain\"")) != NULL)
        cfg.gain = strtol(p + 7, NULL, 10);
    if ((p = strstr(json, "\"start_angle\"")) != NULL)
        cfg.start_angle = strtol(p + 14, NULL, 10);
    if ((p = strstr(json, "\"end_angle\"")) != NULL)
        cfg.end_angle = strtol(p + 12, NULL, 10);
    if ((p = strstr(json, "\"num_samples\"")) != NULL)
        cfg.num_samples = strtol(p + 14, NULL, 10);
    if ((p = strstr(json, "\"transmit_frequency\"")) != NULL)
        cfg.transmit_frequency = strtol(p + 21, NULL, 10);
    if ((p = strstr(json, "\"range_mm\"")) != NULL)
        cfg.range_mm = strtol(p + 11, NULL, 10);
    if ((p = strstr(json, "\"saltwater\"")) != NULL)
        cfg.saltwater = (strstr(p, "true") != NULL);

    free(buf);
    ping360_set_config(&cfg);

    /* Broadcast updated config to all clients */
    xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
    for (int i = 0; i < s_ws_count; i++) {
        send_config_to_client(s_ws_fds[i]);
    }
    xSemaphoreGive(s_ws_mutex);

    return ESP_OK;
}

/* ---- Static file serving (embedded in binary) ---- */

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, index_html_start, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t css_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/css");
    httpd_resp_send(req, style_css_start, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t js_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_send(req, sonar_js_start, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* HTTPS catch-all: redirect unknown paths to root */
static esp_err_t https_catchall_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* ---- Session close callback ---- */

static void session_close_cb(void *ctx, int fd)
{
    (void)ctx;
    xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
    ws_remove_client(fd);
    close(fd);
    xSemaphoreGive(s_ws_mutex);
}

/* ---- Public API ---- */

esp_err_t web_server_init(void)
{
    s_ws_mutex = xSemaphoreCreateMutex();
    if (!s_ws_mutex) {
        ESP_LOGE(TAG, "Failed to create WS mutex");
        return ESP_ERR_NO_MEM;
    }

    /* ---- Start HTTPS server (port 443) — main app ---- */
    httpd_ssl_config_t ssl_config = HTTPD_SSL_CONFIG_DEFAULT();
    ssl_config.httpd.max_open_sockets = 4;
    ssl_config.httpd.max_uri_handlers = 6;
    ssl_config.httpd.lru_purge_enable = true;
    ssl_config.httpd.close_fn = session_close_cb;
    ssl_config.httpd.uri_match_fn = httpd_uri_match_wildcard;
    ssl_config.httpd.send_wait_timeout = 1;
    ssl_config.servercert = server_pem_start;
    ssl_config.servercert_len = server_pem_end - server_pem_start;
    ssl_config.prvtkey_pem = server_key_start;
    ssl_config.prvtkey_len = server_key_end - server_key_start;

    esp_err_t ret = httpd_ssl_start(&s_server, &ssl_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HTTPS server start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Register HTTPS URI handlers */
    httpd_uri_t uri_index = {
        .uri = "/", .method = HTTP_GET,
        .handler = index_handler,
    };
    httpd_register_uri_handler(s_server, &uri_index);

    httpd_uri_t uri_css = {
        .uri = "/style.css", .method = HTTP_GET,
        .handler = css_handler,
    };
    httpd_register_uri_handler(s_server, &uri_css);

    httpd_uri_t uri_js = {
        .uri = "/sonar.js", .method = HTTP_GET,
        .handler = js_handler,
    };
    httpd_register_uri_handler(s_server, &uri_js);

    httpd_uri_t uri_ws = {
        .uri = "/ws", .method = HTTP_GET,
        .handler = ws_handler,
        .is_websocket = true,
    };
    httpd_register_uri_handler(s_server, &uri_ws);

    /* HTTPS catch-all: redirect unknown paths to root (must be last) */
    httpd_uri_t uri_https_catchall = {
        .uri = "/*", .method = HTTP_GET,
        .handler = https_catchall_handler,
    };
    httpd_register_uri_handler(s_server, &uri_https_catchall);

    ESP_LOGI(TAG, "HTTPS server started on port 443");

    return ESP_OK;
}

esp_err_t web_server_broadcast_sonar(uint16_t angle, const uint8_t *data,
                                     uint16_t num_samples)
{
    if (!s_server) return ESP_OK;

    size_t payload_len = 5 + num_samples;
    s_sonar_payload_buf[0] = 0x01;  /* sonar type tag */
    s_sonar_payload_buf[1] = angle & 0xFF;
    s_sonar_payload_buf[2] = (angle >> 8) & 0xFF;
    s_sonar_payload_buf[3] = num_samples & 0xFF;
    s_sonar_payload_buf[4] = (num_samples >> 8) & 0xFF;
    memcpy(s_sonar_payload_buf + 5, data, num_samples);

    httpd_ws_frame_t frame = {
        .final = true,
        .type = HTTPD_WS_TYPE_BINARY,
        .payload = s_sonar_payload_buf,
        .len = payload_len,
    };

    xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
    for (int i = 0; i < s_ws_count; ) {
        esp_err_t ret = httpd_ws_send_frame_async(s_server, s_ws_fds[i], &frame);
        if (ret != ESP_OK) {
            s_ws_send_fail++;
            ESP_LOGW(TAG, "WS send failed fd=%d, closing", s_ws_fds[i]);
            httpd_sess_trigger_close(s_server, s_ws_fds[i]);
            ws_remove_client(s_ws_fds[i]);
        } else {
            s_ws_send_ok++;
            i++;
        }
    }
    xSemaphoreGive(s_ws_mutex);

    return ESP_OK;
}

esp_err_t web_server_broadcast_status(float depth_m, float temp_c,
                                      float pressure_mbar, int batt_mv,
                                      float scan_rate, bool sonar_connected)
{
    if (!s_server) return ESP_OK;

    /* Snapshot and reset debug counters */
    uint32_t send_ok = s_ws_send_ok;
    uint32_t send_fail = s_ws_send_fail;
    s_ws_send_ok = 0;
    s_ws_send_fail = 0;

    char buf[384];
    int len = snprintf(buf, sizeof(buf),
        "{\"type\":\"status\",\"depth_m\":%.2f,\"temp_c\":%.1f,"
        "\"pressure_mbar\":%.1f,\"batt_mv\":%d,\"wifi_clients\":%d,"
        "\"scan_rate\":%.1f,\"sonar_connected\":%s,"
        "\"ws_ok\":%lu,\"ws_fail\":%lu}",
        depth_m, temp_c, pressure_mbar, batt_mv,
        s_ws_count, scan_rate,
        sonar_connected ? "true" : "false",
        (unsigned long)send_ok, (unsigned long)send_fail);

    if (send_fail > 0) {
        ESP_LOGW(TAG, "WS stats: ok=%lu fail=%lu",
                 (unsigned long)send_ok, (unsigned long)send_fail);
    }

    httpd_ws_frame_t frame = {
        .final = true,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)buf,
        .len = len,
    };

    xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
    for (int i = 0; i < s_ws_count; ) {
        esp_err_t err = httpd_ws_send_frame_async(s_server, s_ws_fds[i], &frame);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "WS status send failed fd=%d, closing", s_ws_fds[i]);
            httpd_sess_trigger_close(s_server, s_ws_fds[i]);
            ws_remove_client(s_ws_fds[i]);
        } else {
            i++;
        }
    }
    xSemaphoreGive(s_ws_mutex);

    return ESP_OK;
}

bool web_server_check_zero_depth(void)
{
    if (s_zero_depth_requested) {
        s_zero_depth_requested = false;
        return true;
    }
    return false;
}
