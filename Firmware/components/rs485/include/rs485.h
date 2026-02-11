#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#define RS485_TX_PIN  25
#define RS485_RX_PIN  27
#define RS485_DE_PIN  26
#define RS485_UART_NUM 1
#define RS485_BAUD_RATE 115200

esp_err_t rs485_init(void);
int rs485_send(const uint8_t *data, size_t len);
int rs485_recv(uint8_t *buf, size_t max_len, uint32_t timeout_ms);
void rs485_flush_rx(void);
