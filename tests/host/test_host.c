/**
 * @file    test_host.c
 * @brief   Host unit tests for the pure-C modules.
 *
 * Compiles and runs on a desktop gcc — no ESP32, no STM32, no FreeRTOS.
 * Covered: crc16 (canonical CCITT-FALSE vectors), packet (roundtrip,
 * resync, corruption), history_ring (ordering, overwrite, window),
 * json_util (escaping, truncation, body scanners).
 *
 * Run:  make && ./test_host
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "crc16.h"
#include "packet.h"
#include "history_ring.h"
#include "json_util.h"

static int s_pass = 0, s_fail = 0;

#define CHECK(cond, name)                                        \
    do {                                                          \
        if (cond) { s_pass++; printf("  ok   %s\n", name); }      \
        else      { s_fail++; printf("  FAIL %s\n", name); }      \
    } while (0)

/* ================================================================== */
/*  CRC vectors (CRC-16/CCITT-FALSE, computed independently)          */
/* ================================================================== */

static void test_crc(void)
{
    printf("[crc16]\n");
    struct { const char *data; uint16_t want; } const v[] = {
        { "",               0xFFFF },
        { "123456789",      0x29B1 },
        { "A",              0xB915 },
        { "health-monitor", 0x1E45 },
    };
    for (unsigned i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
        uint16_t got = crc16_ccitt((const uint8_t *)v[i].data,
                                   strlen(v[i].data));
        char name[64];
        snprintf(name, sizeof(name), "vector \"%s\" = 0x%04X",
                 v[i].data, v[i].want);
        CHECK(got == v[i].want, name);
    }
}

/* ================================================================== */
/*  Packet codec                                                      */
/* ================================================================== */

static void test_packet_roundtrip(void)
{
    printf("[packet roundtrip]\n");

    uint8_t payload[20];
    for (int i = 0; i < 20; i++) {
        payload[i] = (uint8_t)(i * 7 + 3);
    }

    uint8_t wire[PACKET_MAX_SIZE];
    int n = packet_encode(wire, sizeof(wire), MSG_TELEMETRY, 0xABCDu,
                          payload, sizeof(payload));
    CHECK(n == (int)(sizeof(payload) + PACKET_OVERHEAD_SIZE),
          "encoded size = payload + 8");

    packet_decoder_t dec;
    packet_decoder_init(&dec);
    packet_result_t r = PACKET_NEED_MORE;
    int mid_ok = 1;
    for (int i = 0; i < n - 1; i++) {
        r = packet_decode_byte(&dec, wire[i]);
        if (r != PACKET_NEED_MORE) {
            mid_ok = 0;
        }
    }
    CHECK(mid_ok, "mid-stream returns NEED_MORE");
    r = packet_decode_byte(&dec, wire[n - 1]);
    CHECK(r == PACKET_OK, "final byte -> PACKET_OK");
    CHECK(dec.decoded.type == MSG_TELEMETRY, "type preserved");
    CHECK(dec.decoded.seq == 0xABCDu, "seq preserved");
    CHECK(dec.decoded.payload_len == sizeof(payload), "length preserved");
    CHECK(memcmp(dec.decoded.payload, payload, sizeof(payload)) == 0,
          "payload preserved");
    CHECK(dec.decoded.crc_valid, "crc flagged valid");
}

static void test_packet_resync(void)
{
    printf("[packet resync]\n");

    uint8_t payload[] = { 0x01, 0x02, 0x03 };
    uint8_t wire[PACKET_MAX_SIZE];
    int n = packet_encode(wire, sizeof(wire), MSG_ALARM, 42u,
                          payload, sizeof(payload));
    CHECK(n > 0, "encode ok");

    packet_decoder_t dec;
    packet_decoder_init(&dec);

    /* 20 garbage bytes first: decoder must skip them all. */
    int garbage_clean = 1;
    for (int g = 0; g < 20; g++) {
        packet_result_t r = packet_decode_byte(&dec, (uint8_t)(g | 0x80));
        if (r == PACKET_OK) {
            garbage_clean = 0;
        }
    }
    CHECK(garbage_clean, "garbage bytes consumed (no packet)");

    packet_result_t r = PACKET_NEED_MORE;
    for (int i = 0; i < n; i++) {
        r = packet_decode_byte(&dec, wire[i]);
    }
    CHECK(r == PACKET_OK, "clean packet decoded after garbage");
    CHECK(dec.decoded.seq == 42u, "seq after resync");
}

static void test_packet_corruption(void)
{
    printf("[packet corruption]\n");

    uint8_t payload[] = { 0xAA, 0x55, 0x00, 0xFF };
    uint8_t wire[PACKET_MAX_SIZE];
    int n = packet_encode(wire, sizeof(wire), MSG_HEARTBEAT, 7u,
                          payload, sizeof(payload));
    CHECK(n > 0, "encode ok");

    /* Flip a payload byte: CRC must fail. */
    uint8_t bad[PACKET_MAX_SIZE] = { 0 };
    memcpy(bad, wire, (size_t)n);
    bad[6] ^= 0x40u;
    packet_decoder_t dec;
    packet_decoder_init(&dec);
    packet_result_t r = PACKET_NEED_MORE;
    for (int i = 0; i < n; i++) {
        r = packet_decode_byte(&dec, bad[i]);
    }
    CHECK(r == PACKET_ERR_CRC, "corrupted payload -> ERR_CRC");

    /* Flip the footer: framing error. */
    memcpy(bad, wire, (size_t)n);
    bad[n - 1] ^= 0xFFu;
    packet_decoder_init(&dec);
    for (int i = 0; i < n; i++) {
        r = packet_decode_byte(&dec, bad[i]);
    }
    CHECK(r == PACKET_ERR_FOOTER, "bad footer -> ERR_FOOTER");

    /* Length field beyond the max: ERR_LENGTH. */
    memcpy(bad, wire, (size_t)n);
    bad[1] = (uint8_t)(PACKET_MAX_PAYLOAD + 1);
    packet_decoder_init(&dec);
    r = packet_decode_byte(&dec, bad[0]);
    r = packet_decode_byte(&dec, bad[1]);
    CHECK(r == PACKET_ERR_LENGTH, "oversize length -> ERR_LENGTH");

    /* Encoder guards. */
    CHECK(packet_encode(NULL, 10, MSG_HEARTBEAT, 0, NULL, 0) == -1,
          "NULL buffer -> -1");
    CHECK(packet_encode(wire, sizeof(wire), MSG_HEARTBEAT, 0, NULL,
                        PACKET_MAX_PAYLOAD + 1) == -2,
          "oversize payload -> -2");
    CHECK(packet_encode(wire, 8, MSG_HEARTBEAT, 0, payload,
                        sizeof(payload)) == -3,
          "small capacity -> -3");
}

/* ================================================================== */
/*  history_ring                                                      */
/* ================================================================== */

static void test_history_ring(void)
{
    printf("[history_ring]\n");

    hr_sample_t storage[8];
    history_ring_t r;
    history_ring_init(&r, storage, 8);

    CHECK(history_ring_count(&r) == 0, "empty at start");
    CHECK(history_ring_at(&r, 0) == NULL, "at() on empty = NULL");

    float vals[HR_CHANNELS] = { 1.0f, 2.0f, 3.0f };
    for (uint32_t t = 100; t <= 800; t += 100) {
        vals[0] = (float)t / 100.0f;
        history_ring_push(&r, t, vals);
    }
    CHECK(history_ring_count(&r) == 8, "count = capacity");
    CHECK(history_ring_at(&r, 0)->t_ms == 100, "oldest first (idx 0)");
    CHECK(history_ring_at(&r, 7)->t_ms == 800, "newest last (idx 7)");
    CHECK(history_ring_at(&r, 8) == NULL, "out-of-range = NULL");

    history_ring_push(&r, 900, vals);            /* overwrites oldest */
    CHECK(history_ring_count(&r) == 8, "count stays at capacity");
    CHECK(history_ring_at(&r, 0)->t_ms == 200, "oldest advanced after wrap");
    CHECK(history_ring_at(&r, 7)->t_ms == 900, "newest is the push");

    hr_sample_t out[16];
    CHECK(history_ring_copy_since(&r, 500, out, 16) == 5,
          "copy_since(500) -> 5 samples");
    CHECK(out[0].t_ms == 500 && out[4].t_ms == 900,
          "copy_since ordering oldest->newest");
    CHECK(history_ring_copy_since(&r, 0, out, 2) == 2,
          "copy_since respects max");
    CHECK(history_ring_copy_since(&r, 99999, out, 16) == 0,
          "copy_since beyond newest = 0");
}

/* ================================================================== */
/*  json_util                                                         */
/* ================================================================== */

static void test_json_writer(void)
{
    printf("[json_util writer]\n");

    char buf[64];
    jsonw_t w;
    jsonw_init(&w, buf, sizeof(buf));
    jsonw_raw(&w, "{\"s\":");
    jsonw_str(&w, "a\"b\\c\nd");
    jsonw_raw(&w, ",\"n\":");
    jsonw_num(&w, 24.5);
    jsonw_raw(&w, ",\"i\":");
    jsonw_int(&w, -1234567890123LL);
    jsonw_raw(&w, ",\"b\":");
    jsonw_bool(&w, 1);
    jsonw_raw(&w, "}");
    CHECK(strcmp(buf,
        "{\"s\":\"a\\\"b\\\\c\\nd\",\"n\":24.50,\"i\":-1234567890123,"
        "\"b\":true}") == 0, "full writer output exact");

    /* Escaping of control characters. */
    jsonw_init(&w, buf, sizeof(buf));
    jsonw_str(&w, "\x01\x02");
    CHECK(strcmp(buf, "\"\\u0001\\u0002\"") == 0, "control chars escaped");

    /* Truncation: everything stays NUL-terminated and bounded. */
    char small[8];
    jsonw_init(&w, small, sizeof(small));
    jsonw_str(&w, "0123456789ABCDEFGHIJKLMNOP");
    CHECK(strlen(small) < sizeof(small), "truncated stays in bounds");
    CHECK(jsonw_len(&w) == 2 + 26, "virtual length reports the overflow");
}

static void test_json_scanners(void)
{
    printf("[json_util scanners]\n");

    const char *body = "{\"id\":2,\"value\":12.5,\"on\":1,"
                       "\"ssid\":\"my net\\\"5G\",\"pass\":\"p@ss\"}";
    long long id = -1, on = 0;
    double value = 0;
    char ssid[32];

    CHECK(json_find_long(body, "id", &id) == 0 && id == 2, "find long id");
    CHECK(json_find_long(body, "on", &on) == 0 && on == 1, "find long on");
    CHECK(json_find_double(body, "value", &value) == 0 && value > 12.49
          && value < 12.51, "find double value");
    CHECK(json_find_str(body, "ssid", ssid, sizeof(ssid)) == 0
          && strcmp(ssid, "my net\"5G") == 0, "find string + unescape");
    CHECK(json_find_long(body, "missing", &id) == -1, "missing key -> -1");
    CHECK(json_find_long("{\"id\":true}", "id", &id) == -1,
          "non-numeric -> -1");

    /* Key-in-key false positive guard: "id" must not match "ssid". */
    CHECK(json_find_long("{\"ssid\":9,\"id\":1}", "id", &id) == 0
          && id == 1, "substring key not matched");

    /* urlencoded bodies. */
    char out[32];
    const char *form = "ssid=My+Home%20Net&pass=abc%40def";
    CHECK(urlform_find(form, "ssid", out, sizeof(out)) == 0
          && strcmp(out, "My Home Net") == 0, "urlform + and %20");
    CHECK(urlform_find(form, "pass", out, sizeof(out)) == 0
          && strcmp(out, "abc@def") == 0, "urlform %40");
    CHECK(urlform_find(form, "user", out, sizeof(out)) == -1,
          "urlform missing -> -1");
    CHECK(urlform_find(form, "ssid", out, 4) == -2,
          "urlform too small -> -2");
}

/* ================================================================== */

int main(void)
{
    test_crc();
    test_packet_roundtrip();
    test_packet_resync();
    test_packet_corruption();
    test_history_ring();
    test_json_writer();
    test_json_scanners();

    printf("\n%d passed, %d failed\n", s_pass, s_fail);
    return (s_fail == 0) ? 0 : 1;
}
