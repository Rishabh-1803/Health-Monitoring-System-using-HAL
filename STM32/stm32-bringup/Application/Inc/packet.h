#ifndef PACKET_H
#define PACKET_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "protocol_types.h"

typedef enum {
    PACKET_OK            =  0,
    PACKET_NEED_MORE     =  1,
    PACKET_ERR_HEADER    = -1,
    PACKET_ERR_LENGTH    = -2,
    PACKET_ERR_FOOTER    = -3,
    PACKET_ERR_CRC       = -4,
    PACKET_ERR_OVERFLOW  = -5,
} packet_result_t;

int packet_encode(uint8_t *out_buf, size_t out_cap,
                  msg_type_t type, uint16_t seq,
                  const uint8_t *payload, uint8_t payload_len);

typedef struct {
    uint8_t  state;
    uint8_t  length;
    uint8_t  type;
    uint16_t seq;
    uint8_t  payload_buf[PACKET_MAX_PAYLOAD];
    uint8_t  payload_idx;
    uint16_t crc_recv;
    uint8_t  crc_idx;
    uint8_t  footer;
    decoded_packet_t decoded;
} packet_decoder_t;

void packet_decoder_init(packet_decoder_t *dec);
packet_result_t packet_decode_byte(packet_decoder_t *dec, uint8_t byte);
void packet_decoder_reset(packet_decoder_t *dec);

#endif /* PACKET_H */