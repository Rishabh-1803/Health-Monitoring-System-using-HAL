/**
 * @file    http_server.c
 * @brief   HTTP server implementation.
 *
 * Static assets are compiled into the firmware image (EMBEDFILES in
 * CMakeLists.txt) — no filesystem dependency, no LittleFS mount order,
 * and the dashboard works on a bare bench board the instant WiFi is up.
 *
 * Design notes:
 *   - REST handlers that command the STM32 block up to ~900 ms (the
 *     command dispatcher's ACK budget). httpd serialises handlers, so
 *     the 1 Hz WebSocket push and REST polls queue behind them rather
 *     than racing — acceptable on a single-instrument dashboard.
 *   - The history endpoint decimates server-side (<= 720 points) and
 *     streams in 4 KB chunks, so peak memory stays ~12 KB regardless
 *     of how full the one-hour ring is.
 *   - JSON bodies are parsed with the fixed-key scanners in json_util;
 *     the browser sends exactly the shapes these handlers look for.
 */

#include "http_server.h"
#include "dashboard_data.h"
#include "command_dispatcher.h"
#include "wifi_manager.h"
#include "websocket_server.h"
#include "json_util.h"
#include "project_config.h"

#include "esp_log.h"
#include "esp_system.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <stdlib.h>

static const char *TAG = "HTTP_SRV";

static httpd_handle_t s_server;

/* Embedded web assets (see main/CMakeLists.txt EMBEDFILES). */
extern const uint8_t index_html_start[] asm("_binary_Web_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_Web_index_html_end");
extern const uint8_t style_css_start[]  asm("_binary_Web_style_css_start");
extern const uint8_t style_css_end[]    asm("_binary_Web_style_css_end");
extern const uint8_t app_js_start[]     asm("_binary_Web_app_js_start");
extern const uint8_t app_js_end[]       asm("_binary_Web_app_js_end");

/* ================================================================== */
/*  Small helpers                                                     */
/* ================================================================== */

static esp_err_t send_file(httpd_req_t *req, const char *type,
                            const uint8_t *start, const uint8_t *end)
{
    httpd_resp_set_type(req, type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)start,
                           (size_t)(end - start));
}

static esp_err_t send_ok(httpd_req_t *req, bool ok, const char *err)
{
    char body[96];
    if (ok) {
        snprintf(body, sizeof(body), "{\"ok\":true}");
    } else {
        snprintf(body, sizeof(body), "{\"ok\":false,\"err\":\"%s\"}",
                 (err != NULL) ? err : "failed");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

/** Read the whole body (bounded) into a stack/heap buffer. */
static char *read_body(httpd_req_t *req, size_t max_len)
{
    size_t total = req->content_len;
    if (total == 0u || total > max_len) {
        return NULL;
    }
    char *buf = malloc(total + 1u);
    if (buf == NULL) {
        return NULL;
    }
    size_t got = 0;
    while (got < total) {
        int n = httpd_req_recv(req, &buf[got], (int)(total - got));
        if (n <= 0) {
            free(buf);
            return NULL;
        }
        got += (size_t)n;
    }
    buf[total] = '\0';
    return buf;
}

/* HTTP budget for one acknowledged command (3 tries x 300 ms). */
#define CMD_HTTP_TIMEOUT_MS  900

/* ================================================================== */
/*  Static routes                                                     */
/* ================================================================== */

static esp_err_t h_index(httpd_req_t *req)
{
    return send_file(req, "text/html", index_html_start, index_html_end);
}

static esp_err_t h_style(httpd_req_t *req)
{
    return send_file(req, "text/css", style_css_start, style_css_end);
}

static esp_err_t h_app_js(httpd_req_t *req)
{
    return send_file(req, "application/javascript",
                     app_js_start, app_js_end);
}

/* Captive portals probe URLs; answer everything unknown with a
 * redirect to the root so the OS opens the setup page itself. */
static esp_err_t h_captive(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, NULL, 0);
}

/* ================================================================== */
/*  REST: status + history                                            */
/* ================================================================== */

static esp_err_t h_api_status(httpd_req_t *req)
{
    char buf[2400];
    size_t n = dashboard_data_build_status_json(buf, sizeof(buf));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, (n < sizeof(buf)) ? (int)n : (int)sizeof(buf) - 1);
}

/* History: ?win=<seconds>, default 300, clamped to 60..3600.
 * Decimated to <= HIST_MAX_POINTS and streamed in 1 KB chunks. */
#define HIST_MAX_POINTS  720u

static esp_err_t h_api_history(httpd_req_t *req)
{
    long long win_s = 300;
    char q[32] = { 0 };
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        char *v = strchr(q, '=');
        if (v != NULL) {
            win_s = strtoll(v + 1, NULL, 10);
        }
    }
    if (win_s < 60)    { win_s = 60; }
    if (win_s > 3600)  { win_s = 3600; }

    uint32_t now = dashboard_data_uptime_ms();
    uint32_t win_ms = (uint32_t)win_s * 1000u;
    uint32_t since = (now > win_ms) ? (now - win_ms) : 0u;

    hr_sample_t *samples = malloc(sizeof(hr_sample_t) * HIST_MAX_POINTS);
    if (samples == NULL) {
        return send_ok(req, false, "oom");
    }
    uint16_t count = dashboard_data_copy_history_since(since, samples,
                                                       HIST_MAX_POINTS);
    uint32_t step = 1u;
    if (count > 0u) {
        step = ((uint32_t)count + HIST_MAX_POINTS - 1u) / HIST_MAX_POINTS;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    (void)httpd_resp_send_chunk(req, "{\"type\":\"history\",\"win\":", -1);
    char num[32];
    snprintf(num, sizeof(num), "%lld", win_s);
    (void)httpd_resp_send_chunk(req, num, -1);
    (void)httpd_resp_send_chunk(req, ",\"now\":", -1);
    snprintf(num, sizeof(num), "%lu", (unsigned long)now);
    (void)httpd_resp_send_chunk(req, num, -1);
    (void)httpd_resp_send_chunk(req, ",\"pts\":[", -1);

    char chunk[1024];
    size_t used = 0;
    bool first = true;
    for (uint32_t i = 0; i < (uint32_t)count; i += step) {
        int n = snprintf(&chunk[used], sizeof(chunk) - used,
                         "%s[%lu,%.2f,%.3f,%.3f]",
                         first ? "" : ",",
                         (unsigned long)samples[i].t_ms,
                         (double)samples[i].v[0],
                         (double)samples[i].v[1],
                         (double)samples[i].v[2]);
        first = false;
        if (n < 0 || (size_t)n >= sizeof(chunk) - used) {
            (void)httpd_resp_send_chunk(req, chunk, (int)used);
            used = 0;
            n = snprintf(chunk, sizeof(chunk), "[%lu,%.2f,%.3f,%.3f]",
                         (unsigned long)samples[i].t_ms,
                         (double)samples[i].v[0],
                         (double)samples[i].v[1],
                         (double)samples[i].v[2]);
            if (n < 0) {
                n = 0;
            }
        }
        used += (size_t)n;
    }
    if (used > 0u) {
        (void)httpd_resp_send_chunk(req, chunk, (int)used);
    }
    (void)httpd_resp_send_chunk(req, "]}", -1);
    (void)httpd_resp_send_chunk(req, NULL, 0);    /* end chunked body   */
    free(samples);
    return ESP_OK;
}

/* ================================================================== */
/*  REST: commands (all funnel through the dispatcher)                */
/* ================================================================== */

static esp_err_t h_api_threshold(httpd_req_t *req)
{
    char *body = read_body(req, 128);
    if (body == NULL) {
        return send_ok(req, false, "bad body");
    }
    long long id = -1;
    double value = 0.0;
    bool ok = (json_find_long(body, "id", &id) == 0)
              && (json_find_double(body, "value", &value) == 0)
              && (id >= 0) && (id <= 2);
    free(body);

    if (!ok) {
        return send_ok(req, false, "bad args");
    }
    cmd_result_t r = command_set_threshold((uint8_t)id, (float)value,
                                           CMD_HTTP_TIMEOUT_MS);
    if (r == CMD_OK) {
        dashboard_data_note_threshold((uint8_t)id, (float)value);
    }
    return send_ok(req, r == CMD_OK,
                   (r == CMD_NAKED) ? "rejected by stm32"
                                    : "no ack from stm32");
}

static esp_err_t h_api_rate(httpd_req_t *req)
{
    char *body = read_body(req, 64);
    if (body == NULL) {
        return send_ok(req, false, "bad body");
    }
    long long ms = 0;
    bool ok = (json_find_long(body, "ms", &ms) == 0)
              && (ms >= 50) && (ms <= 1000);
    free(body);

    if (!ok) {
        return send_ok(req, false, "ms must be 50..1000");
    }
    cmd_result_t r = command_set_sample_rate((uint16_t)ms,
                                             CMD_HTTP_TIMEOUT_MS);
    if (r == CMD_OK) {
        dashboard_data_note_sample_rate((uint16_t)ms);
    }
    return send_ok(req, r == CMD_OK, "no ack from stm32");
}

static esp_err_t h_api_led(httpd_req_t *req)
{
    char *body = read_body(req, 64);
    if (body == NULL) {
        return send_ok(req, false, "bad body");
    }
    long long id = -1, on = 0;
    bool ok = (json_find_long(body, "id", &id) == 0)
              && (json_find_long(body, "on", &on) == 0)
              && (id >= 0) && (id <= 3);
    free(body);

    if (!ok) {
        return send_ok(req, false, "bad args");
    }
    cmd_result_t r = command_led((uint8_t)id, (on != 0),
                                 CMD_HTTP_TIMEOUT_MS);
    return send_ok(req, r == CMD_OK, "no ack from stm32");
}

static esp_err_t h_api_alarm_reset(httpd_req_t *req)
{
    cmd_result_t r = command_alarm_reset(CMD_HTTP_TIMEOUT_MS);
    return send_ok(req, r == CMD_OK, "no ack from stm32");
}

static esp_err_t h_api_reboot_stm32(httpd_req_t *req)
{
    cmd_result_t r = command_reboot_stm32(CMD_HTTP_TIMEOUT_MS);
    return send_ok(req, r == CMD_OK, "no ack from stm32");
}

/* ================================================================== */
/*  REST: WiFi provisioning                                           */
/* ================================================================== */

static esp_err_t h_api_wifi_save(httpd_req_t *req)
{
    /* Accept both JSON {"ssid":"..","pass":".."} and a classic
     * urlencoded form (captive portals often submit the latter). */
    char ssid[33] = { 0 };
    char pass[65] = { 0 };
    char *body = read_body(req, 256);

    if (body != NULL && strstr(body, "application/json") == NULL
        && strchr(body, '=') != NULL && strchr(body, ':') == NULL) {
        /* urlencoded */
        if (urlform_find(body, "ssid", ssid, sizeof(ssid)) != 0
            || urlform_find(body, "pass", pass, sizeof(pass)) != 0) {
            free(body);
            return send_ok(req, false, "missing fields");
        }
    } else if (body != NULL) {
        if (json_find_str(body, "ssid", ssid, sizeof(ssid)) != 0
            || json_find_str(body, "pass", pass, sizeof(pass)) != 0) {
            free(body);
            return send_ok(req, false, "missing fields");
        }
    }
    free(body);

    if (ssid[0] == '\0') {
        return send_ok(req, false, "empty ssid");
    }

    httpd_resp_set_type(req, "application/json");
    (void)httpd_resp_send(req,
        "{\"ok\":true,\"msg\":\"saved — rebooting into station mode\"}",
        HTTPD_RESP_USE_STRLEN);

    wifi_manager_save_credentials(ssid, pass);   /* reboots */
    return ESP_OK;
}

static esp_err_t h_api_wifi_state(httpd_req_t *req)
{
    char body[128];
    char ip[16], ssid[33];
    wifi_manager_get_ip(ip, sizeof(ip));
    wifi_manager_get_ssid(ssid, sizeof(ssid));
    snprintf(body, sizeof(body),
             "{\"state\":%d,\"ip\":\"%s\",\"ssid\":\"%s\"}",
             (int)wifi_manager_get_state(), ip, ssid);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

/* ================================================================== */
/*  Server assembly                                                   */
/* ================================================================== */

bool http_server_running(void)
{
    return s_server != NULL;
}

esp_err_t http_server_start(void)
{
    if (s_server != NULL) {
        return ESP_OK;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.max_uri_handlers = 16;
    cfg.server_port = 80;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: 0x%x", err);
        return err;
    }
    websocket_server_set_httpd(s_server);

    static const httpd_uri_t routes[] = {
        { .uri = "/",            .method = HTTP_GET,  .handler = h_index },
        { .uri = "/style.css",   .method = HTTP_GET,  .handler = h_style },
        { .uri = "/app.js",      .method = HTTP_GET,  .handler = h_app_js },
        { .uri = "/api/status",  .method = HTTP_GET,  .handler = h_api_status },
        { .uri = "/api/history", .method = HTTP_GET,  .handler = h_api_history },
        { .uri = "/api/threshold", .method = HTTP_POST, .handler = h_api_threshold },
        { .uri = "/api/rate",    .method = HTTP_POST, .handler = h_api_rate },
        { .uri = "/api/led",     .method = HTTP_POST, .handler = h_api_led },
        { .uri = "/api/alarm/reset", .method = HTTP_POST, .handler = h_api_alarm_reset },
        { .uri = "/api/reboot-stm32", .method = HTTP_POST, .handler = h_api_reboot_stm32 },
        { .uri = "/api/wifi/save",  .method = HTTP_POST, .handler = h_api_wifi_save },
        { .uri = "/api/wifi/state", .method = HTTP_GET,  .handler = h_api_wifi_state },
        { .uri = "/generate_204",   .method = HTTP_GET,  .handler = h_captive },
        { .uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = h_captive },
        { .uri = "/ncsi.txt",       .method = HTTP_GET,  .handler = h_captive },
        { .uri = "/favicon.ico",    .method = HTTP_GET,  .handler = h_captive },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        (void)httpd_register_uri_handler(s_server, &routes[i]);
    }

    static const httpd_uri_t ws_route = {
        .uri = "/ws", .method = HTTP_GET,
        .handler = websocket_server_ws_handler,
        .is_websocket = true,
    };
    (void)httpd_register_uri_handler(s_server, &ws_route);

    ESP_LOGI(TAG, "HTTP+WS server on port 80 (%d URI handlers)",
             (int)(sizeof(routes) / sizeof(routes[0]) + 1));
    return ESP_OK;
}
