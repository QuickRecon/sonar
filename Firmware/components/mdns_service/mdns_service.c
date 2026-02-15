#include "mdns_service.h"
#include "mdns.h"
#include "esp_log.h"

static const char *TAG = "mdns_service";

esp_err_t mdns_service_init(void)
{
    esp_err_t ret = mdns_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mDNS init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = mdns_hostname_set("sonar");
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mDNS hostname set failed: %s", esp_err_to_name(ret));
        return ret;
    }

    mdns_instance_name_set("SonarMK2");

    mdns_service_add("SonarMK2 Web", "_https", "_tcp", 443, NULL, 0);
    mdns_service_add("SonarMK2 Web", "_http", "_tcp", 80, NULL, 0);

    ESP_LOGI(TAG, "mDNS started: sonar.local");
    return ESP_OK;
}
