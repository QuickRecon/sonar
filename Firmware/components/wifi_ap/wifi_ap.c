#include "wifi_ap.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <string.h>

/* ---- DEBUG: uncomment to connect to existing AP instead of creating one ---- */
//#define WIFI_DEBUG_STA_MODE
#define WIFI_DEBUG_STA_SSID     "Aren Apartment"
#define WIFI_DEBUG_STA_PASS     "5590712748"
/* ---------------------------------------------------------------------------- */

static const char *TAG = "wifi_ap";

static int s_station_count = 0;
static esp_netif_t *s_netif = NULL;

#ifdef WIFI_DEBUG_STA_MODE

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

static void sta_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Disconnected, reconnecting...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

#else

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        s_station_count++;
        ESP_LOGI(TAG, "Station connected (total=%d)", s_station_count);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        if (s_station_count > 0) s_station_count--;
        ESP_LOGI(TAG, "Station disconnected (total=%d)", s_station_count);
    }
}

#endif

#ifdef WIFI_DEBUG_STA_MODE

esp_err_t wifi_ap_init(void)
{
    s_wifi_event_group = xEventGroupCreate();
    s_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) return ret;

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        &sta_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        &sta_event_handler, NULL, NULL);

    wifi_config_t wifi_cfg = {
        .sta = {
            .ssid = WIFI_DEBUG_STA_SSID,
            .password = WIFI_DEBUG_STA_PASS,
        },
    };

    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) return ret;

    ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    if (ret != ESP_OK) return ret;

    ret = esp_wifi_start();
    if (ret != ESP_OK) return ret;

    /* Wait for connection (up to 10s) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
        WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(10000));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to AP: %s", WIFI_DEBUG_STA_SSID);
    } else {
        ESP_LOGW(TAG, "Failed to connect to AP: %s (continuing anyway)",
                 WIFI_DEBUG_STA_SSID);
    }

    return ESP_OK;
}

#else

esp_err_t wifi_ap_init(void)
{
    s_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) return ret;

    ret = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              &wifi_event_handler, NULL, NULL);
    if (ret != ESP_OK) return ret;

    wifi_config_t wifi_cfg = {
        .ap = {
            .ssid = CONFIG_SONARMK2_WIFI_SSID,
            .ssid_len = strlen(CONFIG_SONARMK2_WIFI_SSID),
            .channel = CONFIG_SONARMK2_WIFI_CHANNEL,
            .max_connection = CONFIG_SONARMK2_MAX_STA_CONN,
            .authmode = WIFI_AUTH_OPEN,
            .pmf_cfg = {
                .required = false,
            },
        },
    };

    /* If password is set, use WPA2 */
    if (strlen(CONFIG_SONARMK2_WIFI_PASSWORD) > 0) {
        strlcpy((char *)wifi_cfg.ap.password, CONFIG_SONARMK2_WIFI_PASSWORD,
                sizeof(wifi_cfg.ap.password));
        wifi_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    }

    ret = esp_wifi_set_mode(WIFI_MODE_AP);
    if (ret != ESP_OK) return ret;

    ret = esp_wifi_set_config(WIFI_IF_AP, &wifi_cfg);
    if (ret != ESP_OK) return ret;

    ret = esp_wifi_start();
    if (ret != ESP_OK) return ret;

    ESP_LOGI(TAG, "WiFi AP started (SSID: %s)", CONFIG_SONARMK2_WIFI_SSID);
    return ESP_OK;
}

#endif

esp_err_t wifi_ap_get_ip(char *buf, size_t len)
{
    if (!s_netif || !buf) return ESP_ERR_INVALID_ARG;

    esp_netif_ip_info_t ip_info;
    esp_err_t ret = esp_netif_get_ip_info(s_netif, &ip_info);
    if (ret != ESP_OK) return ret;

    snprintf(buf, len, IPSTR, IP2STR(&ip_info.ip));
    return ESP_OK;
}

int wifi_ap_get_station_count(void)
{
    return s_station_count;
}
