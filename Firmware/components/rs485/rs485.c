#include "rs485.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "rs485";

#define RS485_RX_BUF_SIZE  2048

esp_err_t rs485_init(void)
{
    uart_config_t uart_cfg = {
        .baud_rate = RS485_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t ret = uart_param_config(RS485_UART_NUM, &uart_cfg);
    if (ret != ESP_OK) return ret;

    ret = uart_set_pin(RS485_UART_NUM, RS485_TX_PIN, RS485_RX_PIN,
                       RS485_DE_PIN, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) return ret;

    ret = uart_driver_install(RS485_UART_NUM, RS485_RX_BUF_SIZE, 0, 0,
                              NULL, 0);
    if (ret != ESP_OK) return ret;

    ret = uart_set_mode(RS485_UART_NUM, UART_MODE_RS485_HALF_DUPLEX);
    if (ret != ESP_OK) return ret;

    ESP_LOGI(TAG, "RS-485 initialized (TX=%d RX=%d DE=%d @ %d baud)",
             RS485_TX_PIN, RS485_RX_PIN, RS485_DE_PIN, RS485_BAUD_RATE);
    return ESP_OK;
}

int rs485_send(const uint8_t *data, size_t len)
{
    return uart_write_bytes(RS485_UART_NUM, data, len);
}

int rs485_recv(uint8_t *buf, size_t max_len, uint32_t timeout_ms)
{
    return uart_read_bytes(RS485_UART_NUM, buf, max_len,
                           pdMS_TO_TICKS(timeout_ms));
}

void rs485_flush_rx(void)
{
    uart_flush_input(RS485_UART_NUM);
}
