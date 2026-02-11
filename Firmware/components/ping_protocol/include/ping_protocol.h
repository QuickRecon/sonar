#pragma once

#include <stddef.h>
#include <stdint.h>

#define PING_MSG_GENERAL_REQUEST   6
#define PING_MSG_PROTOCOL_VERSION  5
#define PING_MSG_TRANSDUCER        2601
#define PING_MSG_DEVICE_DATA       2300
#define PING_MSG_MOTOR_OFF         2903

#define PING_FRAME_HEADER_LEN  8
#define PING_FRAME_CHECKSUM_LEN 2
#define PING_MAX_PAYLOAD_LEN   1210
#define PING_MAX_FRAME_LEN     (PING_FRAME_HEADER_LEN + PING_MAX_PAYLOAD_LEN + PING_FRAME_CHECKSUM_LEN)

#define PING_SRC_ID  0
#define PING_DST_ID  1

typedef struct {
    uint8_t  mode;
    uint8_t  gain;
    uint16_t angle;
    uint16_t transmit_duration;
    uint16_t sample_period;
    uint16_t transmit_frequency;
    uint16_t num_samples;
    uint8_t  transmit;
} ping_transducer_cmd_t;

typedef struct {
    uint8_t  mode;
    uint8_t  gain;
    uint16_t angle;
    uint16_t transmit_duration;
    uint16_t sample_period;
    uint16_t transmit_frequency;
    uint16_t num_samples;
    const uint8_t *data;
    uint16_t data_len;
} ping_device_data_t;

typedef enum {
    PING_PARSE_START1,
    PING_PARSE_START2,
    PING_PARSE_LEN_L,
    PING_PARSE_LEN_H,
    PING_PARSE_ID_L,
    PING_PARSE_ID_H,
    PING_PARSE_SRC,
    PING_PARSE_DST,
    PING_PARSE_PAYLOAD,
    PING_PARSE_CHK_L,
    PING_PARSE_CHK_H,
} ping_parse_state_t;

typedef struct {
    ping_parse_state_t state;
    uint8_t buf[PING_MAX_FRAME_LEN];
    uint16_t payload_len;
    uint16_t msg_id;
    uint16_t payload_idx;
    uint16_t checksum;
    uint16_t rx_checksum;
} ping_parser_t;

int ping_build_frame(uint8_t *buf, size_t buf_size, uint16_t msg_id,
                     const uint8_t *payload, uint16_t payload_len);
int ping_build_transducer_cmd(uint8_t *buf, size_t buf_size,
                              const ping_transducer_cmd_t *cmd);
int ping_build_general_request(uint8_t *buf, size_t buf_size,
                               uint16_t requested_id);
int ping_build_motor_off(uint8_t *buf, size_t buf_size);

void ping_parser_init(ping_parser_t *parser);
int ping_parser_feed(ping_parser_t *parser, uint8_t byte);
int ping_parse_device_data(const ping_parser_t *parser,
                           ping_device_data_t *out);
