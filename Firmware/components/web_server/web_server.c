#include "web_server.h"
#include "ping360.h"
#include "esp_http_server.h"
#include "esp_spiffs.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static const char *TAG = "web_server";

static httpd_handle_t s_server = NULL;

#define MAX_WS_CLIENTS 4

static int s_ws_fds[MAX_WS_CLIENTS];
static int s_ws_count = 0;

/* ---- WebSocket client management ---- */

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

static void send_config_to_client(int fd)
{
    ping360_config_t cfg;
    ping360_get_config(&cfg);

    char buf[256];
    int len = snprintf(buf, sizeof(buf),
        "{\"type\":\"config\",\"gain\":%u,\"start_angle\":%u,"
        "\"end_angle\":%u,\"num_samples\":%u,"
        "\"transmit_frequency\":%u,\"transmit_duration\":%u,"
        "\"sample_period\":%u,\"range_mm\":%lu,\"speed_of_sound\":%u}",
        cfg.gain, cfg.start_angle, cfg.end_angle, cfg.num_samples,
        cfg.transmit_frequency, cfg.transmit_duration,
        cfg.sample_period, (unsigned long)cfg.range_mm, cfg.speed_of_sound);

    httpd_ws_frame_t ws_frame = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)buf,
        .len = len,
    };
    httpd_ws_send_frame_async(s_server, fd, &ws_frame);
}

/* ---- WebSocket handler ---- */

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        int fd = httpd_req_to_sockfd(req);
        ws_add_client(fd);
        send_config_to_client(fd);
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
    if ((p = strstr(json, "\"transmit_duration\"")) != NULL)
        cfg.transmit_duration = strtol(p + 20, NULL, 10);
    if ((p = strstr(json, "\"sample_period\"")) != NULL)
        cfg.sample_period = strtol(p + 16, NULL, 10);
    if ((p = strstr(json, "\"range_mm\"")) != NULL)
        cfg.range_mm = strtol(p + 11, NULL, 10);
    if ((p = strstr(json, "\"speed_of_sound\"")) != NULL)
        cfg.speed_of_sound = strtol(p + 17, NULL, 10);

    free(buf);
    ping360_set_config(&cfg);

    /* Broadcast updated config to all clients */
    for (int i = 0; i < s_ws_count; i++) {
        send_config_to_client(s_ws_fds[i]);
    }

    return ESP_OK;
}

/* ---- Static file serving ---- */

static esp_err_t serve_file(httpd_req_t *req, const char *path,
                            const char *content_type)
{
    char filepath[64];
    snprintf(filepath, sizeof(filepath), "/www/%s", path);

    FILE *f = fopen(filepath, "r");
    if (!f) {
        ESP_LOGE(TAG, "File not found: %s", filepath);
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, content_type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    char chunk[512];
    size_t read_bytes;
    while ((read_bytes = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        httpd_resp_send_chunk(req, chunk, read_bytes);
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t index_handler(httpd_req_t *req)
{
    return serve_file(req, "index.html", "text/html");
}

static esp_err_t css_handler(httpd_req_t *req)
{
    return serve_file(req, "style.css", "text/css");
}

static esp_err_t js_handler(httpd_req_t *req)
{
    return serve_file(req, "sonar.js", "application/javascript");
}

/* ---- Captive portal / NCSI handlers ---- */

/* Android connectivity check — expects 204 */
static esp_err_t handle_generate_204(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* Windows NCSI — expects 200 with specific body */
static esp_err_t handle_connecttest(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "Microsoft Connect Test", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* Windows NCSI IPv4/IPv6 probe — expects 200 */
static esp_err_t handle_success_txt(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* Apple CNA — expects 200 with "Success" in body */
static esp_err_t handle_hotspot_detect(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, "<HTML><HEAD><TITLE>Success</TITLE></HEAD>"
                         "<BODY>Success</BODY></HTML>",
                    HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* Catch-all: redirect unknown URLs to the sonar UI */
static esp_err_t captive_redirect_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* ---- Session close callback ---- */

static void session_close_cb(void *ctx, int fd)
{
    /* Note: ctx is the httpd server handle (not session context).
     * Session context cleanup is handled by httpd_sess_clear_ctx()
     * which runs after this callback returns. */
    (void)ctx;
    ws_remove_client(fd);
    close(fd);
}

/* ---- Sonar broadcast work function ---- */

typedef struct {
    uint16_t angle;
    uint16_t num_samples;
    uint8_t data[];
} sonar_broadcast_t;

static void sonar_broadcast_work(void *arg)
{
    sonar_broadcast_t *bc = arg;
    size_t frame_len = 5 + bc->num_samples;
    uint8_t *frame = malloc(frame_len);
    if (!frame) {
        free(bc);
        return;
    }

    frame[0] = 0x01;
    frame[1] = bc->angle & 0xFF;
    frame[2] = (bc->angle >> 8) & 0xFF;
    frame[3] = bc->num_samples & 0xFF;
    frame[4] = (bc->num_samples >> 8) & 0xFF;
    memcpy(&frame[5], bc->data, bc->num_samples);

    httpd_ws_frame_t ws_frame = {
        .type = HTTPD_WS_TYPE_BINARY,
        .payload = frame,
        .len = frame_len,
    };

    for (int i = 0; i < s_ws_count; i++) {
        httpd_ws_send_frame_async(s_server, s_ws_fds[i], &ws_frame);
    }

    free(frame);
    free(bc);
}

/* ---- Status broadcast work function ---- */

typedef struct {
    float depth_m;
    float temp_c;
    float pressure_mbar;
    int batt_mv;
    float scan_rate;
    bool sonar_connected;
} status_broadcast_t;

static void status_broadcast_work(void *arg)
{
    status_broadcast_t *st = arg;

    char buf[256];
    int len = snprintf(buf, sizeof(buf),
        "{\"type\":\"status\",\"depth_m\":%.2f,\"temp_c\":%.1f,"
        "\"pressure_mbar\":%.1f,\"batt_mv\":%d,\"wifi_clients\":%d,"
        "\"scan_rate\":%.1f,\"sonar_connected\":%s}",
        st->depth_m, st->temp_c, st->pressure_mbar, st->batt_mv,
        s_ws_count, st->scan_rate,
        st->sonar_connected ? "true" : "false");

    httpd_ws_frame_t ws_frame = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)buf,
        .len = len,
    };

    for (int i = 0; i < s_ws_count; i++) {
        httpd_ws_send_frame_async(s_server, s_ws_fds[i], &ws_frame);
    }

    free(st);
}

/* ---- Public API ---- */

esp_err_t web_server_init(void)
{
    /* Mount SPIFFS */
    esp_vfs_spiffs_conf_t spiffs_cfg = {
        .base_path = "/www",
        .partition_label = "storage",
        .max_files = 5,
        .format_if_mount_failed = false,
    };
    esp_err_t ret = esp_vfs_spiffs_register(&spiffs_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(ret));
        return ret;
    }

    size_t total = 0, used = 0;
    esp_spiffs_info("storage", &total, &used);
    ESP_LOGI(TAG, "SPIFFS: total=%zu used=%zu", total, used);

    /* Start HTTP server */
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 7;
    config.max_uri_handlers = 12;
    config.lru_purge_enable = true;
    config.close_fn = session_close_cb;
    config.uri_match_fn = httpd_uri_match_wildcard;

    ret = httpd_start(&s_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Register URI handlers */
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

    /* OS connectivity check endpoints — return "internet works" responses
     * so the OS doesn't deprioritize the WiFi interface */
    httpd_uri_t uri_generate_204 = {
        .uri = "/generate_204", .method = HTTP_GET,
        .handler = handle_generate_204,
    };
    httpd_register_uri_handler(s_server, &uri_generate_204);

    httpd_uri_t uri_connecttest = {
        .uri = "/connecttest.txt", .method = HTTP_GET,
        .handler = handle_connecttest,
    };
    httpd_register_uri_handler(s_server, &uri_connecttest);

    httpd_uri_t uri_success = {
        .uri = "/success.txt", .method = HTTP_GET,
        .handler = handle_success_txt,
    };
    httpd_register_uri_handler(s_server, &uri_success);

    httpd_uri_t uri_hotspot = {
        .uri = "/hotspot-detect.html", .method = HTTP_GET,
        .handler = handle_hotspot_detect,
    };
    httpd_register_uri_handler(s_server, &uri_hotspot);

    /* Catch-all: redirect any other URL to the sonar UI (must be last) */
    httpd_uri_t uri_catchall = {
        .uri = "/*", .method = HTTP_GET,
        .handler = captive_redirect_handler,
    };
    httpd_register_uri_handler(s_server, &uri_catchall);

    ESP_LOGI(TAG, "Web server started");
    return ESP_OK;
}

esp_err_t web_server_broadcast_sonar(uint16_t angle, const uint8_t *data,
                                     uint16_t num_samples)
{
    if (!s_server || s_ws_count == 0) return ESP_OK;

    sonar_broadcast_t *bc = malloc(sizeof(sonar_broadcast_t) + num_samples);
    if (!bc) return ESP_ERR_NO_MEM;

    bc->angle = angle;
    bc->num_samples = num_samples;
    memcpy(bc->data, data, num_samples);

    return httpd_queue_work(s_server, sonar_broadcast_work, bc);
}

esp_err_t web_server_broadcast_status(float depth_m, float temp_c,
                                      float pressure_mbar, int batt_mv,
                                      float scan_rate, bool sonar_connected)
{
    if (!s_server || s_ws_count == 0) return ESP_OK;

    status_broadcast_t *st = malloc(sizeof(status_broadcast_t));
    if (!st) return ESP_ERR_NO_MEM;

    st->depth_m = depth_m;
    st->temp_c = temp_c;
    st->pressure_mbar = pressure_mbar;
    st->batt_mv = batt_mv;
    st->scan_rate = scan_rate;
    st->sonar_connected = sonar_connected;

    return httpd_queue_work(s_server, status_broadcast_work, st);
}
