#include "canbus.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "canbus";

static twai_node_handle_t s_node = NULL;

esp_err_t canbus_init(void)
{
    twai_onchip_node_config_t node_cfg = {
        .io_cfg = {
            .tx = CANBUS_TX_PIN,
            .rx = CANBUS_RX_PIN,
            .quanta_clk_out = -1,
            .bus_off_indicator = -1,
        },
        .bit_timing = {
            .bitrate = CANBUS_BITRATE,
        },
        .fail_retry_cnt = -1,
        .tx_queue_depth = 5,
    };

    esp_err_t ret = twai_new_node_onchip(&node_cfg, &s_node);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TWAI node create failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "CAN bus initialized (TX=%d RX=%d @ %dkbps)",
             CANBUS_TX_PIN, CANBUS_RX_PIN, CANBUS_BITRATE / 1000);
    return ESP_OK;
}

esp_err_t canbus_start(void)
{
    if (!s_node) return ESP_ERR_INVALID_STATE;
    return twai_node_enable(s_node);
}

esp_err_t canbus_stop(void)
{
    if (!s_node) return ESP_ERR_INVALID_STATE;
    return twai_node_disable(s_node);
}

esp_err_t canbus_send(uint32_t id, const uint8_t *data, size_t len)
{
    if (!s_node) return ESP_ERR_INVALID_STATE;
    if (len > 8) return ESP_ERR_INVALID_ARG;

    uint8_t tx_data[8] = {0};
    memcpy(tx_data, data, len);

    twai_frame_t frame = {
        .header = {
            .id = id,
            .dlc = len,
        },
        .buffer = tx_data,
        .buffer_len = len,
    };

    return twai_node_transmit(s_node, &frame, 100);
}

esp_err_t canbus_deinit(void)
{
    if (!s_node) return ESP_OK;
    esp_err_t ret = twai_node_delete(s_node);
    if (ret == ESP_OK) s_node = NULL;
    return ret;
}
