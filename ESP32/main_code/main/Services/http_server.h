/**
 * @file    http_server.h
 * @brief   HTTP routes: embedded dashboard + REST + WebSocket upgrade.
 */

#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <stdint.h>
#include <stdbool.h>

#include "esp_http_server.h"

/**
 * Start the HTTP server (idempotent: a second call is a no-op).
 * Safe to call in station mode (after an IP exists) or while the
 * provisioning AP is up — the dashboard page adapts to either state.
 *
 * @return ESP_OK, or an error from httpd_start().
 */
esp_err_t http_server_start(void);

/** True once the server is running. */
bool http_server_running(void);

#endif /* HTTP_SERVER_H */
