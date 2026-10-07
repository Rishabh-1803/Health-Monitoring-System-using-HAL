/**
 * @file    thingspeak.c
 * @brief   ThingSpeak uploader (plain HTTP POST to api.thingspeak.com).
 *
 * Plain HTTP keeps RAM use small (no TLS on a board with no PSRAM
 * enabled). The write key therefore travels unencrypted: use a channel
 * you can afford to rotate.
 */

#include "thingspeak.h"
#include "dashboard_data.h"
#include "wifi_manager.h"
#include "logger.h"

#include "esp_http_client.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "THINGSPEAK";

#include "thingspeak_config.h"

static const char *s_key = THINGSPEAK_WRITE_API_KEY;
static uint32_t s_ok, s_fail;
static long     s_last_entry;
static int      s_last_http;
static char     s_last_err[40] = "not sent yet";
static SemaphoreHandle_t s_lock;

static bool key_valid(void)
{
    if (s_key == NULL || s_key[0] == '\0'
        || strcmp(s_key, "PASTE_YOUR_WRITE_API_KEY_HERE") == 0) {
        return false;
    }
    return true;
}

void thingspeak_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    ESP_LOGI(TAG, "%s", key_valid()
             ? "write key set in code - uploading enabled"
             : "no key - edit Services/thingspeak_config.h");
}

bool thingspeak_enabled(void)
{
    return key_valid();
}

void thingspeak_get_key_masked(char *buf, size_t cap)
{
    if (buf == NULL || cap == 0u) {
        return;
    }
    size_t n = key_valid() ? strlen(s_key) : 0u;
    if (n == 0u) {
        buf[0] = '\0';
    } else if (n <= 4u) {
        snprintf(buf, cap, "****");
    } else {
        snprintf(buf, cap, "************%s", &s_key[n - 4u]);
    }
}

/* Collect the (tiny) response body: ThingSpeak answers with the entry id. */
static char s_body[16];
static esp_err_t http_evt(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_DATA && e->data != NULL
        && e->data_len > 0) {
        size_t have = strlen(s_body);
        size_t room = sizeof(s_body) - 1u - have;
        size_t n = ((size_t)e->data_len < room) ? (size_t)e->data_len : room;
        memcpy(&s_body[have], e->data, n);
        s_body[have + n] = '\0';
    }
    return ESP_OK;
}

static bool upload_locked(void);

bool thingspeak_upload_now(void)
{
    if (!thingspeak_enabled()) {
        return false;
    }
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(12000)) != pdTRUE) {
        return false;
    }
    bool ok = upload_locked();
    xSemaphoreGive(s_lock);
    return ok;
}

static bool upload_locked(void)
{
    if (wifi_manager_get_state() != WIFI_MGR_CONNECTED) {
        snprintf(s_last_err, sizeof(s_last_err), "no WiFi (station)");
        return false;
    }
    dash_snapshot_t s;
    dashboard_data_snapshot(&s);

    static char post[256];     /* static: keep it off the task stack */
    snprintf(post, sizeof(post),
             "api_key=%s&field1=%.2f&field2=%.2f&field3=%.3f&field4=%u"
             "&field5=%.1f&field6=%d&field7=%d&field8=%u",
             s_key,
             (double)s.temp_c,
             (double)s.current_a * 1000.0,
             (double)s.vib_g,
             (unsigned)s.alarm_bits,
             (double)s.stm32_cpu_10000 / 100.0,
             wifi_manager_get_rssi(),
             s.link_up ? 1 : 0,
             (unsigned)s.sensor_status);

    s_body[0] = '\0';
    esp_http_client_config_t cfg = {
        .url = "http://api.thingspeak.com/update",
        .method = HTTP_METHOD_POST,
        .timeout_ms = 8000,
        .event_handler = http_evt,
        .buffer_size = 512,
        .buffer_size_tx = 512,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) {
        snprintf(s_last_err, sizeof(s_last_err), "client init failed");
        s_fail++;
        return false;
    }
    esp_http_client_set_header(c, "Content-Type",
                               "application/x-www-form-urlencoded");
    esp_http_client_set_post_field(c, post, (int)strlen(post));

    esp_err_t err = esp_http_client_perform(c);
    s_last_http = (err == ESP_OK) ? esp_http_client_get_status_code(c) : 0;
    esp_http_client_cleanup(c);

    long entry = strtol(s_body, NULL, 10);
    bool ok = (err == ESP_OK) && (s_last_http == 200) && (entry > 0);
    if (ok) {
        s_ok++;
        s_last_entry = entry;
        snprintf(s_last_err, sizeof(s_last_err), "ok");
    } else {
        s_fail++;
        if (err != ESP_OK) {
            snprintf(s_last_err, sizeof(s_last_err), "network: %s",
                     esp_err_to_name(err));
        } else if (s_last_http == 200 && entry == 0) {
            snprintf(s_last_err, sizeof(s_last_err),
                     "rejected (bad key or <15 s since last)");
        } else {
            snprintf(s_last_err, sizeof(s_last_err), "HTTP %d", s_last_http);
        }
        ESP_LOGW(TAG, "upload failed: %s", s_last_err);
    }
    return ok;
}

void thingspeak_status_json(char *buf, size_t cap)
{
    char masked[THINGSPEAK_KEY_MAX + 1];
    thingspeak_get_key_masked(masked, sizeof(masked));
    snprintf(buf, cap,
             "{\"enabled\":%s,\"key\":\"%s\",\"ok\":%lu,\"fail\":%lu,"
             "\"entry\":%ld,\"http\":%d,\"msg\":\"%s\"}",
             thingspeak_enabled() ? "true" : "false", masked,
             (unsigned long)s_ok, (unsigned long)s_fail,
             s_last_entry, s_last_http, s_last_err);
}
