/**
 * @file    protocol_selftest.c
 * @brief   Protocol self-test for ESP32 — same 8 tests as STM32.
 *
 * Runs at boot BEFORE starting the heartbeat tasks, so the protocol
 * layer is proven on the ESP32 before it exchanges real packets with
 * the STM32.
 *
 * Uses ESP_LOGI for output (thread-safe on ESP32).
 */

#include "protocol_selftest.h"
#include "crc16.h"
#include "protocol_types.h"
#include "packet.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "SELFTEST";

/* Helper: print via ESP_LOGI (strip \r\n from self-test messages) */
static void log_line(const char *msg)
{
    /* ESP_LOGI already adds a newline; strip any \r\n from our messages */
    char buf[256];
    strncpy(buf, msg, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    /* Strip trailing \r\n */
    int len = strlen(buf);
    while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r')) {
        buf[--len] = '\0';
    }
    if (len > 0) {
        ESP_LOGI(TAG, "%s", buf);
    }
}

/* Same tests as STM32 — code is deliberately identical in logic */
static bool test_crc_vectors(void)
{
    log_line("[T1] CRC test vectors");

    struct {
        const char *name;
        const char *data_ascii;
        uint16_t    expected;
    } const cases[] = {
        { "empty",       "",           0xFFFF },
        { "123456789",   "123456789",  0x29B1 },
        { "A",           "A",          0xB915 },
    };

    for (unsigned i = 0; i < (sizeof(cases) / sizeof(cases[0])); i++) {
        uint16_t got = crc16_ccitt((const uint8_t *)cases[i].data_ascii,
                                    strlen(cases[i].data_ascii));
        if (got != cases[i].expected) {
            char buf[128];
            snprintf(buf, sizeof(buf), "  FAIL: %s -> expected 0x%04X, got 0x%04X",
                     cases[i].name, cases[i].expected, got);
            log_line(buf);
            return false;
        }
        char buf[128];
        snprintf(buf, sizeof(buf), "  PASS: %s -> 0x%04X", cases[i].name, got);
        log_line(buf);
    }
    return true;
}

static bool test_struct_sizes(void)
{
    log_line("[T2] Struct sizes");

    struct {
        const char *name;
        size_t      actual;
        size_t      expected;
    } const checks[] = {
        { "telemetry_payload_t",           sizeof(telemetry_payload_t),           20 },
        { "alarm_payload_t",               sizeof(alarm_payload_t),                8 },
        { "ack_payload_t",                 sizeof(ack_payload_t),                  4 },
        { "nak_payload_t",                 sizeof(nak_payload_t),                  4 },
        { "cmd_set_threshold_payload_t",   sizeof(cmd_set_threshold_payload_t),    8 },
        { "cmd_set_sample_rate_payload_t", sizeof(cmd_set_sample_rate_payload_t), 4 },
        { "cmd_led_payload_t",             sizeof(cmd_led_payload_t),              2 },
        { "resp_status_payload_t",         sizeof(resp_status_payload_t),          32 },
        { "reset_payload_t",               sizeof(reset_payload_t),                1 },
    };

    bool all_ok = true;
    for (unsigned i = 0; i < (sizeof(checks) / sizeof(checks[0])); i++) {
        if (checks[i].actual != checks[i].expected) {
            char buf[128];
            snprintf(buf, sizeof(buf), "  FAIL: %-35s expected %u, got %u",
                     checks[i].name, (unsigned)checks[i].expected, (unsigned)checks[i].actual);
            log_line(buf);
            all_ok = false;
        } else {
            char buf[128];
            snprintf(buf, sizeof(buf), "  PASS: %-35s %u bytes",
                     checks[i].name, (unsigned)checks[i].actual);
            log_line(buf);
        }
    }
    return all_ok;
}

static bool encode_decode_loopback(msg_type_t type, uint16_t seq,
                                   const uint8_t *payload, uint8_t payload_len,
                                   const char *test_name)
{
    uint8_t tx_buf[PACKET_MAX_SIZE];
    int encoded_len = packet_encode(tx_buf, sizeof(tx_buf),
                                     type, seq, payload, payload_len);
    if (encoded_len <= 0) {
        char buf[128];
        snprintf(buf, sizeof(buf), "  FAIL: %s -- encode returned %d", test_name, encoded_len);
        log_line(buf);
        return false;
    }

    packet_decoder_t dec;
    packet_decoder_init(&dec);

    packet_result_t result = PACKET_NEED_MORE;
    for (int i = 0; i < encoded_len; i++) {
        result = packet_decode_byte(&dec, tx_buf[i]);
        if (result == PACKET_OK) break;
    }

    if (result != PACKET_OK) {
        char buf[128];
        snprintf(buf, sizeof(buf), "  FAIL: %s -- decode returned %d", test_name, result);
        log_line(buf);
        return false;
    }
    if (dec.decoded.type != type || dec.decoded.seq != seq ||
        dec.decoded.payload_len != payload_len) {
        char buf[128];
        snprintf(buf, sizeof(buf), "  FAIL: %s -- field mismatch", test_name);
        log_line(buf);
        return false;
    }
    if (payload_len > 0 && memcmp(dec.decoded.payload, payload, payload_len) != 0) {
        char buf[128];
        snprintf(buf, sizeof(buf), "  FAIL: %s -- payload mismatch", test_name);
        log_line(buf);
        return false;
    }

    char buf[128];
    snprintf(buf, sizeof(buf), "  PASS: %s (encoded=%d bytes)", test_name, encoded_len);
    log_line(buf);
    return true;
}

static bool test_crc_corruption(void)
{
    log_line("[T6] CRC corruption detection");

    uint8_t tx_buf[PACKET_MAX_SIZE];
    int len = packet_encode(tx_buf, sizeof(tx_buf), MSG_HEARTBEAT, 42, NULL, 0);
    if (len <= 0) return false;

    tx_buf[3] ^= 0xFF;   /* corrupt seq byte */

    packet_decoder_t dec;
    packet_decoder_init(&dec);
    packet_result_t result = PACKET_NEED_MORE;
    for (int i = 0; i < len; i++) {
        result = packet_decode_byte(&dec, tx_buf[i]);
        if (result == PACKET_OK || result == PACKET_ERR_CRC) break;
    }

    if (result != PACKET_ERR_CRC) {
        log_line("  FAIL: expected PACKET_ERR_CRC");
        return false;
    }
    log_line("  PASS: corrupted byte correctly rejected");
    return true;
}

static bool test_resync(void)
{
    log_line("[T7] Resync after garbage bytes");

    uint8_t tx_buf[PACKET_MAX_SIZE];
    int len = packet_encode(tx_buf, sizeof(tx_buf), MSG_HEARTBEAT, 7, NULL, 0);
    if (len <= 0) return false;

    packet_decoder_t dec;
    packet_decoder_init(&dec);

    packet_result_t result = PACKET_NEED_MORE;
    for (int i = 0; i < 10; i++) {
        result = packet_decode_byte(&dec, 0x42);
        if (result != PACKET_ERR_HEADER && result != PACKET_NEED_MORE) return false;
    }
    for (int i = 0; i < len; i++) {
        result = packet_decode_byte(&dec, tx_buf[i]);
        if (result == PACKET_OK) break;
    }

    if (result != PACKET_OK || dec.decoded.seq != 7) {
        log_line("  FAIL: resync failed");
        return false;
    }
    log_line("  PASS: resync after 10 garbage bytes");
    return true;
}

static bool test_seq_wraparound(void)
{
    log_line("[T8] Sequence wraparound");

    uint8_t buf1[PACKET_MAX_SIZE], buf2[PACKET_MAX_SIZE];
    int len1 = packet_encode(buf1, sizeof(buf1), MSG_HEARTBEAT, 0xFFFF, NULL, 0);
    int len2 = packet_encode(buf2, sizeof(buf2), MSG_HEARTBEAT, 0x0001, NULL, 0);
    if (len1 <= 0 || len2 <= 0) return false;

    packet_decoder_t dec;
    packet_decoder_init(&dec);
    packet_result_t result = PACKET_NEED_MORE;
    for (int i = 0; i < len1; i++) {
        result = packet_decode_byte(&dec, buf1[i]);
        if (result == PACKET_OK) break;
    }
    if (result != PACKET_OK || dec.decoded.seq != 0xFFFF) return false;

    packet_decoder_init(&dec);
    for (int i = 0; i < len2; i++) {
        result = packet_decode_byte(&dec, buf2[i]);
        if (result == PACKET_OK) break;
    }
    if (result != PACKET_OK || dec.decoded.seq != 0x0001) return false;

    log_line("  PASS: seq 0xFFFF and 0x0001 both reconstructed correctly");
    return true;
}

bool protocol_selftest_run(void)
{
    bool ok = true;

    log_line("");
    log_line("========================================");
    log_line("  Protocol Self-Test");
    log_line("========================================");

    ok &= test_crc_vectors();
    ok &= test_struct_sizes();

    log_line("[T3-T5] Encode -> Decode loopback");
    ok &= encode_decode_loopback(MSG_HEARTBEAT, 0, NULL, 0, "heartbeat (empty)");

    telemetry_payload_t tel = {
        .temperature_c = 45.5f, .current_a = 2.5f, .vibration_g = 0.8f,
        .alarm_bits = 0x03, .sensor_status = 0x07, .cpu_load_pct = 0,
        .free_heap_bytes_div16 = 256, .uptime_seconds = 5,
    };
    ok &= encode_decode_loopback(MSG_TELEMETRY, 42,
                                  (const uint8_t *)&tel, sizeof(tel),
                                  "telemetry (20 bytes)");

    ack_payload_t ack = { .acked_seq = 42, .acked_type = MSG_TELEMETRY, .reserved = 0 };
    ok &= encode_decode_loopback(MSG_ACK, 100,
                                  (const uint8_t *)&ack, sizeof(ack),
                                  "ACK (4 bytes)");

    cmd_set_threshold_payload_t cmd = {
        .threshold_id = THRESHOLD_ID_TEMPERATURE, .reserved = 0, .value = 55.0f,
    };
    ok &= encode_decode_loopback(MSG_CMD_SET_THRESHOLD, 200,
                                  (const uint8_t *)&cmd, sizeof(cmd),
                                  "CMD_SET_THRESHOLD (8 bytes)");

    cmd_led_payload_t led = { .led_id = LED_ID_RED, .reserved = 0 };
    ok &= encode_decode_loopback(MSG_CMD_LED_ON, 201,
                                  (const uint8_t *)&led, sizeof(led),
                                  "CMD_LED_ON (2 bytes)");

    reset_payload_t rst = { .reset_reason = RESET_REASON_WATCHDOG };
    ok &= encode_decode_loopback(MSG_RESET, 65530,
                                  (const uint8_t *)&rst, sizeof(rst),
                                  "RESET (1 byte)");

    ok &= test_crc_corruption();
    ok &= test_resync();
    ok &= test_seq_wraparound();

    log_line("");
    log_line("----------------------------------------");
    if (ok) {
        log_line("  RESULT: ALL TESTS PASS");
    } else {
        log_line("  RESULT: SOME TESTS FAILED");
    }
    log_line("----------------------------------------");
    log_line("");

    return ok;
}
