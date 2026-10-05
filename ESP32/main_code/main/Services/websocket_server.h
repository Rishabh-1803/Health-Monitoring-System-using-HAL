/**
 * @file    websocket_server.h
 * @brief   WebSocket endpoint + fan-out broadcast.
 *
 * One-way push: the ESP32 streams status JSON to every connected
 * browser at 1 Hz. Client->server traffic is limited to pings and the
 * occasional text echo; browsers issue commands via REST POSTs, which
 * keeps this module free of any command parsing.
 *
 * Sending from another task is safe because frames are queued onto the
 * httpd work queue (httpd_queue_work) and transmitted from the httpd
 * thread — the documented pattern for asynchronous WebSocket sends.
 */

#ifndef WEBSOCKET_SERVER_H
#define WEBSOCKET_SERVER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "esp_http_server.h"

/** Maximum simultaneously connected dashboard clients. */
#define WS_MAX_CLIENTS   8

/**
 * The /ws URI handler. Register from http_server.c:
 *   { .uri = "/ws", .method = HTTP_GET, .handler = websocket_server_ws_handler,
 *     .is_websocket = true }
 */
esp_err_t websocket_server_ws_handler(httpd_req_t *req);

/** Remember the server instance for queued broadcasts (call at start). */
void websocket_server_set_httpd(httpd_handle_t hd);

/**
 * Send a text frame to every connected client. Never blocks the caller
 * longer than the enqueue of one work item per client.
 */
void websocket_server_broadcast(const char *payload, size_t len);

/** Connected client count (for the diagnostics report). */
int websocket_server_client_count(void);

#endif /* WEBSOCKET_SERVER_H */
