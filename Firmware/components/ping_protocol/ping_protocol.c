#include "ping_protocol.h"
#include <string.h>

/* Frame format:
 * 'B' 'R' [u16 LE payload_len] [u16 LE msg_id] [u8 src_id] [u8 dst_id]
 * [payload ...] [u16 LE checksum]
 *
 * Checksum = sum of all bytes before the checksum field (header + payload).
 */

static void write_u16_le(uint8_t *dst, uint16_t val)
{
    dst[0] = (uint8_t)(val & 0xFF);
    dst[1] = (uint8_t)((val >> 8) & 0xFF);
}

static uint16_t read_u16_le(const uint8_t *src)
{
    return (uint16_t)src[0] | ((uint16_t)src[1] << 8);
}

int ping_build_frame(uint8_t *buf, size_t buf_size, uint16_t msg_id,
                     const uint8_t *payload, uint16_t payload_len)
{
    uint16_t total_len = PING_FRAME_HEADER_LEN + payload_len + PING_FRAME_CHECKSUM_LEN;

    if (buf_size < total_len) {
        return -1;
    }

    /* Header */
    buf[0] = 'B';
    buf[1] = 'R';
    write_u16_le(&buf[2], payload_len);
    write_u16_le(&buf[4], msg_id);
    buf[6] = PING_SRC_ID;
    buf[7] = PING_DST_ID;

    /* Payload */
    if (payload_len > 0 && payload != NULL) {
        memcpy(&buf[PING_FRAME_HEADER_LEN], payload, payload_len);
    }

    /* Checksum: sum of all bytes from buf[0] through buf[header_len + payload_len - 1] */
    uint16_t checksum = 0;
    for (uint16_t i = 0; i < PING_FRAME_HEADER_LEN + payload_len; i++) {
        checksum += buf[i];
    }
    write_u16_le(&buf[PING_FRAME_HEADER_LEN + payload_len], checksum);

    return (int)total_len;
}

int ping_build_transducer_cmd(uint8_t *buf, size_t buf_size,
                              const ping_transducer_cmd_t *cmd)
{
    /* Payload: mode(u8) gain(u8) angle(u16) transmit_duration(u16)
     *          sample_period(u16) transmit_frequency(u16) num_samples(u16)
     *          transmit(u8) = 13 bytes */
    uint8_t payload[13];
    uint16_t idx = 0;

    payload[idx++] = cmd->mode;
    payload[idx++] = cmd->gain;
    write_u16_le(&payload[idx], cmd->angle);
    idx += 2;
    write_u16_le(&payload[idx], cmd->transmit_duration);
    idx += 2;
    write_u16_le(&payload[idx], cmd->sample_period);
    idx += 2;
    write_u16_le(&payload[idx], cmd->transmit_frequency);
    idx += 2;
    write_u16_le(&payload[idx], cmd->num_samples);
    idx += 2;
    payload[idx++] = cmd->transmit;

    return ping_build_frame(buf, buf_size, PING_MSG_TRANSDUCER, payload, idx);
}

int ping_build_general_request(uint8_t *buf, size_t buf_size,
                               uint16_t requested_id)
{
    /* Payload: u16 LE requested_id = 2 bytes */
    uint8_t payload[2];
    write_u16_le(payload, requested_id);

    return ping_build_frame(buf, buf_size, PING_MSG_GENERAL_REQUEST, payload, 2);
}

int ping_build_motor_off(uint8_t *buf, size_t buf_size)
{
    return ping_build_frame(buf, buf_size, PING_MSG_MOTOR_OFF, NULL, 0);
}

void ping_parser_init(ping_parser_t *parser)
{
    memset(parser, 0, sizeof(*parser));
    parser->state = PING_PARSE_START1;
}

int ping_parser_feed(ping_parser_t *parser, uint8_t byte)
{
    switch (parser->state) {

    case PING_PARSE_START1:
        if (byte == 'B') {
            parser->buf[0] = byte;
            parser->checksum = byte;
            parser->state = PING_PARSE_START2;
            return 0;
        }
        return -1;

    case PING_PARSE_START2:
        if (byte == 'R') {
            parser->buf[1] = byte;
            parser->checksum += byte;
            parser->state = PING_PARSE_LEN_L;
            return 0;
        }
        /* Not a valid start sequence -- reset */
        parser->state = PING_PARSE_START1;
        return -1;

    case PING_PARSE_LEN_L:
        parser->buf[2] = byte;
        parser->checksum += byte;
        parser->payload_len = byte;
        parser->state = PING_PARSE_LEN_H;
        return 0;

    case PING_PARSE_LEN_H:
        parser->buf[3] = byte;
        parser->checksum += byte;
        parser->payload_len |= ((uint16_t)byte << 8);
        if (parser->payload_len > PING_MAX_PAYLOAD_LEN) {
            parser->state = PING_PARSE_START1;
            return -1;
        }
        parser->state = PING_PARSE_ID_L;
        return 0;

    case PING_PARSE_ID_L:
        parser->buf[4] = byte;
        parser->checksum += byte;
        parser->msg_id = byte;
        parser->state = PING_PARSE_ID_H;
        return 0;

    case PING_PARSE_ID_H:
        parser->buf[5] = byte;
        parser->checksum += byte;
        parser->msg_id |= ((uint16_t)byte << 8);
        parser->state = PING_PARSE_SRC;
        return 0;

    case PING_PARSE_SRC:
        parser->buf[6] = byte;
        parser->checksum += byte;
        parser->state = PING_PARSE_DST;
        return 0;

    case PING_PARSE_DST:
        parser->buf[7] = byte;
        parser->checksum += byte;
        parser->payload_idx = 0;
        if (parser->payload_len == 0) {
            parser->state = PING_PARSE_CHK_L;
        } else {
            parser->state = PING_PARSE_PAYLOAD;
        }
        return 0;

    case PING_PARSE_PAYLOAD:
        parser->buf[PING_FRAME_HEADER_LEN + parser->payload_idx] = byte;
        parser->checksum += byte;
        parser->payload_idx++;
        if (parser->payload_idx >= parser->payload_len) {
            parser->state = PING_PARSE_CHK_L;
        }
        return 0;

    case PING_PARSE_CHK_L:
        parser->rx_checksum = byte;
        parser->buf[PING_FRAME_HEADER_LEN + parser->payload_len] = byte;
        parser->state = PING_PARSE_CHK_H;
        return 0;

    case PING_PARSE_CHK_H:
        parser->rx_checksum |= ((uint16_t)byte << 8);
        parser->buf[PING_FRAME_HEADER_LEN + parser->payload_len + 1] = byte;
        parser->state = PING_PARSE_START1;

        if (parser->rx_checksum == parser->checksum) {
            return 1; /* Complete valid frame */
        }
        return -1; /* Checksum mismatch */

    default:
        parser->state = PING_PARSE_START1;
        return -1;
    }
}

int ping_parse_device_data(const ping_parser_t *parser,
                           ping_device_data_t *out)
{
    if (parser->msg_id != PING_MSG_DEVICE_DATA) {
        return -1;
    }

    /* Device data payload layout:
     * mode(u8) gain(u8) angle(u16) transmit_duration(u16)
     * sample_period(u16) transmit_frequency(u16) num_samples(u16)
     * data[variable]
     *
     * Fixed portion = 1+1+2+2+2+2+2 = 12 bytes
     */
    const uint8_t *payload = &parser->buf[PING_FRAME_HEADER_LEN];
    uint16_t plen = parser->payload_len;

    if (plen < 12) {
        return -1;
    }

    uint16_t idx = 0;
    out->mode = payload[idx++];
    out->gain = payload[idx++];
    out->angle = read_u16_le(&payload[idx]);
    idx += 2;
    out->transmit_duration = read_u16_le(&payload[idx]);
    idx += 2;
    out->sample_period = read_u16_le(&payload[idx]);
    idx += 2;
    out->transmit_frequency = read_u16_le(&payload[idx]);
    idx += 2;
    out->num_samples = read_u16_le(&payload[idx]);
    idx += 2;

    out->data = &payload[idx];
    out->data_len = plen - idx;

    return 0;
}
