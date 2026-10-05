/**
 * @file    wifi_manager.h
 * @brief   WiFi connection manager: STA with retry, AP provisioning.
 *
 * Provisioning model (deliberately simple and robust):
 *   - Credentials live in NVS (namespace "hms_wifi", keys "ssid"/"pass").
 *   - Present  -> station mode; retry up to 10 times with a growing
 *                 backoff; if all fail, fall back to provisioning.
 *   - Absent   -> SoftAP "monitor-setup" (password "12345678") with a
 *                 DNS hijack, so phones/laptops get a captive portal
 *                 that lands on the dashboard, which shows the setup
 *                 form when wifi state == 0.
 *   - Saving credentials writes NVS and reboots — the simplest fully
 *     deterministic way to re-init netif/wifi in the new mode.
 */

#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    WIFI_MGR_PROVISIONING = 0,
    WIFI_MGR_CONNECTING   = 1,
    WIFI_MGR_CONNECTED    = 2,
} wifi_mgr_state_t;

/** Bring up netif + wifi in the mode implied by stored credentials. */
void wifi_manager_init(void);

wifi_mgr_state_t wifi_manager_get_state(void);
const char *wifi_manager_state_str(wifi_mgr_state_t st);

/** Current IP as a string ("" while not connected). */
void wifi_manager_get_ip(char *buf, size_t cap);

/** AP RSSI in dBm (0 when not in station mode). */
int wifi_manager_get_rssi(void);

/** Stored SSID ("" when none). */
void wifi_manager_get_ssid(char *buf, size_t cap);

/**
 * Persist credentials and reboot into station mode.
 * @return true when both were written.
 */
bool wifi_manager_save_credentials(const char *ssid, const char *pass);

/** Erase credentials and reboot into the provisioning AP. */
bool wifi_manager_clear_credentials(void);

/** Refresh the cached RSSI (called by the wifi poller task). */
void wifi_manager_poll_rssi(void);

#endif /* WIFI_MANAGER_H */
