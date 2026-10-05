#include "packet.h"
#include "crc16.h"
#include <string.h>

enum {
    STATE_WAIT_HEADER  = 0,
    STATE_READ_LENGTH  = 1,
    STATE_READ_TYPE    = 2,
    STATE_READ_SEQ     = 3,
    STATE_READ_PAYLOAD = 4,
    STATE_READ_CRC     = 5,
    STATE_READ_FOOTER  = 6,
};

int packet_encode(uint8_t *out_buf, size_t out_cap,
                  msg_type_t type, uint16_t seq,
                  const uint8_t *payload, uint8_t payload_len)
{
    if (out_buf == NULL) return -1;
    if (payload_len > PACKET_MAX_PAYLOAD) return -2;
    size_t total = (size_t)payload_len + PACKET_OVERHEAD_SIZE;
    if (out_cap < total) return -3;

    out_buf[0] = PACKET_HEADER_BYTE;
    out_buf[1] = payload_len;
    out_buf[2] = (uint8_t)type;
    out_buf[3] = (uint8_t)(seq & 0xFF);
    out_buf[4] = (uint8_t)((seq >> 8) & 0xFF);

    if (payload_len > 0) {
        if (payload == NULL) return -1;
        memcpy(&out_buf[5], payload, payload_len);
    }

    size_t crc_len = 5 + payload_len;
    uint16_t crc = crc16_ccitt(out_buf, crc_len);

    out_buf[5 + payload_len]     = (uint8_t)(crc & 0xFF);
    out_buf[5 + payload_len + 1] = (uint8_t)((crc >> 8) & 0xFF);
    out_buf[5 + payload_len + 2] = PACKET_FOOTER_BYTE;

    return (int)total;
}

void packet_decoder_init(packet_decoder_t *dec)
{
    if (dec == NULL) return;
    memset(dec, 0, sizeof(*dec));
    dec->state = STATE_WAIT_HEADER;
}

void packet_decoder_reset(packet_decoder_t *dec)
{
    if (dec == NULL) return;
    dec->state = STATE_WAIT_HEADER;
    dec->length = 0;
    dec->type = 0;
    dec->seq = 0;
    dec->payload_idx = 0;
    dec->crc_idx = 0;
    dec->crc_recv = 0;
    dec->footer = 0;
}

packet_result_t packet_decode_byte(packet_decoder_t *dec, uint8_t byte)
{
    if (dec == NULL) return PACKET_ERR_HEADER;

    switch (dec->state) {

    case STATE_WAIT_HEADER:
        if (byte == PACKET_HEADER_BYTE) {
            dec->state = STATE_READ_LENGTH;
            return PACKET_NEED_MORE;
        }
        return PACKET_ERR_HEADER;

    case STATE_READ_LENGTH:
        if (byte > PACKET_MAX_PAYLOAD) {
            dec->state = STATE_WAIT_HEADER;
            return PACKET_ERR_LENGTH;
        }
        dec->length = byte;
        dec->state = STATE_READ_TYPE;
        return PACKET_NEED_MORE;

    case STATE_READ_TYPE:
        dec->type = byte;
        dec->state = STATE_READ_SEQ;
        dec->crc_idx = 0;
        return PACKET_NEED_MORE;

    case STATE_READ_SEQ:
        if (dec->crc_idx == 0) {
            dec->seq = byte;
            dec->crc_idx = 1;
        } else {
            dec->seq |= ((uint16_t)byte) << 8;
            dec->crc_idx = 0;
            dec->payload_idx = 0;
            dec->state = (dec->length > 0) ? STATE_READ_PAYLOAD : STATE_READ_CRC;
        }
        return PACKET_NEED_MORE;

    case STATE_READ_PAYLOAD:
        dec->payload_buf[dec->payload_idx++] = byte;
        if (dec->payload_idx >= dec->length) {
            dec->state = STATE_READ_CRC;
            dec->crc_idx = 0;
        }
        return PACKET_NEED_MORE;

    case STATE_READ_CRC:
        if (dec->crc_idx == 0) {
            dec->crc_recv = byte;
            dec->crc_idx = 1;
        } else {
            dec->crc_recv |= ((uint16_t)byte) << 8;
            dec->crc_idx = 0;
            dec->state = STATE_READ_FOOTER;
        }
        return PACKET_NEED_MORE;

    case STATE_READ_FOOTER:
        dec->state = STATE_WAIT_HEADER;

        if (byte != PACKET_FOOTER_BYTE) {
            return PACKET_ERR_FOOTER;
        }

        uint8_t crc_buf[PACKET_MAX_PAYLOAD + 5];
        crc_buf[0] = PACKET_HEADER_BYTE;
        crc_buf[1] = dec->length;
        crc_buf[2] = dec->type;
        crc_buf[3] = (uint8_t)(dec->seq & 0xFF);
        crc_buf[4] = (uint8_t)((dec->seq >> 8) & 0xFF);
        if (dec->length > 0) {
            memcpy(&crc_buf[5], dec->payload_buf, dec->length);
        }
        uint16_t crc_calc = crc16_ccitt(crc_buf, 5 + dec->length);

        dec->decoded.type        = (msg_type_t)dec->type;
        dec->decoded.seq         = dec->seq;
        dec->decoded.payload_len = dec->length;
        if (dec->length > 0) {
            memcpy(dec->decoded.payload, dec->payload_buf, dec->length);
        }
        dec->decoded.crc_valid = (crc_calc == dec->crc_recv);

        if (crc_calc != dec->crc_recv) {
            return PACKET_ERR_CRC;
        }

        return PACKET_OK;

    default:
        dec->state = STATE_WAIT_HEADER;
        return PACKET_ERR_HEADER;
    }
}