/**
 * @file    websocket_server.c
 * @brief   WebSocket fan-out implementation.
 *
 * Client lifecycle: the first invocation of the handler for a socket
 * registers its fd (per-session flag via req->sess_ctx). httpd then
 * invokes the handler once per incoming frame; a CLOSE frame (or a
 * failed send) unregisters it. Dead sockets are pruned lazily on the
 * next broadcast — httpd's own select loop reaps the fd, so we only
 * drop it from the registry.
 */

#include "websocket_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#include <string.h>
#include <stdlib.h>

static const char *TAG = "WS_SERVER";

typedef struct {
    int  fd;
    bool in_use;
} ws_client_t;

static ws_client_t s_clients[WS_MAX_CLIENTS];
static SemaphoreHandle_t s_lock;
static httpd_handle_t s_hd;

/* Work item passed to httpd_queue_work — freed by the worker. */
typedef struct {
    int    fd;
    size_t len;
    char   payload[];           /* flexible array, allocated together  */
} ws_work_t;

/* ------------------------------------------------------------------ */

static void registry_add(int fd)
{
    if (s_lock != NULL) { xSemaphoreTake(s_lock, portMAX_DELAY); }
    bool added = false;
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (!s_clients[i].in_use) {
            s_clients[i].fd = fd;
            s_clients[i].in_use = true;
            added = true;
            break;
        }
    }
    if (s_lock != NULL) { xSemaphoreGive(s_lock); }
    if (added) {
        ESP_LOGI(TAG, "client fd=%d connected (%d total)", fd,
                 websocket_server_client_count());
    } else {
        ESP_LOGW(TAG, "client fd=%d rejected: registry full", fd);
    }
}

static void registry_remove(int fd)
{
    if (s_lock != NULL) { xSemaphoreTake(s_lock, portMAX_DELAY); }
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (s_clients[i].in_use && s_clients[i].fd == fd) {
            s_clients[i].in_use = false;
        }
    }
    if (s_lock != NULL) { xSemaphoreGive(s_lock); }
}

int websocket_server_client_count(void)
{
    int n = 0;
    if (s_lock != NULL) { xSemaphoreTake(s_lock, portMAX_DELAY); }
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (s_clients[i].in_use) {
            n++;
        }
    }
    if (s_lock != NULL) { xSemaphoreGive(s_lock); }
    return n;
}

/* ------------------------------------------------------------------ */

void websocket_server_set_httpd(httpd_handle_t hd)
{
    s_hd = hd;
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
}

/* Runs on the httpd thread — httpd_ws_send_frame is legal here. */
static void ws_push_work(void *arg)
{
    ws_work_t *w = (ws_work_t *)arg;
    httpd_ws_frame_t frame = {
        .final = true,
        .fragmented = false,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)w->payload,
        .len = w->len,
    };
    esp_err_t err = httpd_ws_send_frame_async(s_hd, w->fd, &frame);
    if (err != ESP_OK) {
        /* Dead or closing socket: drop it from the registry. httpd
         * reaps the fd itself; nothing else to do. */
        ESP_LOGD(TAG, "send to fd=%d failed (0x%x) — pruning", w->fd, err);
        registry_remove(w->fd);
    }
    free(w);
}

void websocket_server_broadcast(const char *payload, size_t len)
{
    if (s_hd == NULL || payload == NULL || len == 0u) {
        return;
    }
    if (s_lock != NULL) { xSemaphoreTake(s_lock, portMAX_DELAY); }
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (!s_clients[i].in_use) {
            continue;
        }
        ws_work_t *w = malloc(sizeof(*w) + len);
        if (w == NULL) {
            ESP_LOGE(TAG, "broadcast: out of memory");
            break;
        }
        w->fd = s_clients[i].fd;
        w->len = len;
        memcpy(w->payload, payload, len);
        if (httpd_queue_work(s_hd, ws_push_work, w) != ESP_OK) {
            free(w);        /* queue full — skip this client this round */
        }
    }
    if (s_lock != NULL) { xSemaphoreGive(s_lock); }
}

/* ------------------------------------------------------------------ */

/* Per-session context: remembers the fd so the free callback (which
 * httpd invokes when the session ends, close frame or not) can
 * unregister it. */
typedef struct {
    int fd;
} ws_sess_t;

static void sess_ctx_free(void *ctx)
{
    if (ctx != NULL) {
        ws_sess_t *s = (ws_sess_t *)ctx;
        registry_remove(s->fd);
        free(ctx);
    }
}

esp_err_t websocket_server_ws_handler(httpd_req_t *req)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }

    /* First call for this session: register the fd. */
    if (req->sess_ctx == NULL) {
        ws_sess_t *s = calloc(1, sizeof(*s));
        if (s != NULL) {
            s->fd = httpd_req_to_sockfd(req);
            req->sess_ctx = s;
            req->free_ctx = sess_ctx_free;
            registry_add(s->fd);
        }
    }

    /* The handler is called once for the HTTP upgrade (GET) AFTER httpd has
     * already sent the 101 handshake reply. There is no frame to read yet:
     * calling httpd_ws_recv_frame() here would block the single httpd thread
     * for the whole receive timeout (5 s) -- freezing every other request --
     * and then fail, which closes the brand-new socket. That was the
     * "connects, then drops, page never updates" loop. Return immediately,
     * exactly as the ESP-IDF websocket example does. */
    if (req->method == HTTP_GET) {
        return ESP_OK;
    }

    httpd_ws_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    uint8_t buf[128];
    frame.payload = buf;
    /* len MUST be 0 here: a non-zero len tells httpd "header already read"
     * and it then waits for that many payload bytes (until the receive
     * timeout), freezing the server and killing the socket. With 0 it parses
     * the header itself and sets len to the real size. */
    frame.len = 0;

    esp_err_t err = httpd_ws_recv_frame(req, &frame, sizeof(buf));
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "recv failed (0x%x) — client went away", err);
        return err;
    }

    switch (frame.type) {
        case HTTPD_WS_TYPE_TEXT:
            /* Browsers may send a keepalive "ping" text; ignore all. */
            break;

        case HTTPD_WS_TYPE_PING:
            frame.type = HTTPD_WS_TYPE_PONG;
            return httpd_ws_send_frame(req, &frame);

        case HTTPD_WS_TYPE_CLOSE:
            registry_remove(httpd_req_to_sockfd(req));
            /* Returning 0 lets httpd complete the close handshake. */
            return ESP_OK;

        default:
            break;
    }
    return ESP_OK;
}
