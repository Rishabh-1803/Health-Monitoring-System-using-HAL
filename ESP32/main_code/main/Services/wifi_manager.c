/**
 * @file    wifi_manager.c
 * @brief   WiFi manager implementation.
 *
 * The DNS hijack is a hand-rolled UDP:53 responder (~90 lines) rather
 * than an lwip hook: it works on any ESP-IDF version, needs no extra
 * sdkconfig, and answers every A query with the AP address — which is
 * exactly what a captive portal needs.
 */

#include "wifi_manager.h"
#include "project_config.h"
#include "dashboard_data.h"
#include "logger.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "lwip/sockets.h"
#include "lwip/ip4_addr.h"

#include <string.h>

static const char *TAG = "WIFI_MGR";

#define NVS_NAMESPACE   "hms_wifi"
#define NVS_KEY_SSID    "ssid"
#define NVS_KEY_PASS    "pass"

#define STA_MAX_RETRIES     10
#define PROV_SSID           "monitor-setup"
#define PROV_PASS           "12345678"      /* >= 8 chars for WPA2      */
#define PROV_CHANNEL        6
#define PROV_MAX_CONN       4
#define PROV_AP_IP          "192.168.4.1"

/* DNS hijack: answer every A query with the AP address. */
#define DNS_TASK_STACK      4096
#define DNS_PORT            53
#define DNS_MAX_PACKET      512

static wifi_mgr_state_t s_state = WIFI_MGR_CONNECTING;
static char s_ip[16] = "";
static int  s_rssi = 0;
static char s_ssid[33] = "";
static SemaphoreHandle_t s_lock;

static int s_sta_retries = 0;
static esp_netif_t *s_sta_netif;
static bool s_ap_fallback = false;     /* AP running beside a failing STA */
static bool s_dns_running = false;

/* ================================================================== */
/*  Small guarded accessors                                           */
/* ================================================================== */

wifi_mgr_state_t wifi_manager_get_state(void)
{
    return s_state;
}

const char *wifi_manager_state_str(wifi_mgr_state_t st)
{
    switch (st) {
        case WIFI_MGR_PROVISIONING: return "provisioning-AP";
        case WIFI_MGR_CONNECTING:   return "connecting";
        case WIFI_MGR_CONNECTED:    return "connected";
        default:                    return "?";
    }
}

void wifi_manager_get_ip(char *buf, size_t cap)
{
    if (buf == NULL || cap == 0u) {
        return;
    }
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    snprintf(buf, cap, "%s", s_ip);
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}

int wifi_manager_get_rssi(void)
{
    return s_rssi;
}

void wifi_manager_get_ssid(char *buf, size_t cap)
{
    if (buf == NULL || cap == 0u) {
        return;
    }
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    snprintf(buf, cap, "%s", s_ssid);
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}

static void set_state(wifi_mgr_state_t st)
{
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    s_state = st;
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}

/* ================================================================== */
/*  NVS credentials                                                   */
/* ================================================================== */

static bool read_credentials(char *ssid, size_t ssid_cap,
                             char *pass, size_t pass_cap)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = ssid_cap;
    bool ok = (nvs_get_str(h, NVS_KEY_SSID, ssid, &len) == ESP_OK);
    len = pass_cap;
    ok = ok && (nvs_get_str(h, NVS_KEY_PASS, pass, &len) == ESP_OK);
    nvs_close(h);
    return ok;
}

/* Restart from a throw-away task, a moment after the caller returns, so the
 * HTTP reply that triggered it has time to reach the browser (a restart
 * straight after httpd_resp_send often resets the connection before the
 * client has read the answer -> "Failed to fetch"). */
static void restart_task(void *arg)
{
    uint32_t ms = (uint32_t)(uintptr_t)arg;
    vTaskDelay(pdMS_TO_TICKS(ms));
    esp_restart();
}

static void restart_later(uint32_t ms)
{
    if (xTaskCreate(restart_task, "restart", 2048, (void *)(uintptr_t)ms,
                    tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
        vTaskDelay(pdMS_TO_TICKS(ms));
        esp_restart();
    }
}

bool wifi_manager_save_credentials(const char *ssid, const char *pass)
{
    if (ssid == NULL || pass == NULL || ssid[0] == '\0') {
        return false;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    bool ok = (nvs_set_str(h, NVS_KEY_SSID, ssid) == ESP_OK)
              && (nvs_set_str(h, NVS_KEY_PASS, pass) == ESP_OK)
              && (nvs_commit(h) == ESP_OK);
    nvs_close(h);
    if (ok) {
        ESP_LOGI(TAG, "credentials saved — rebooting into station mode");
        restart_later(2000u);             /* after the HTTP reply escapes */
    }
    return ok;
}

bool wifi_manager_clear_credentials(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    bool ok = (nvs_erase_key(h, NVS_KEY_SSID) == ESP_OK);
    nvs_erase_key(h, NVS_KEY_PASS);
    ok = (nvs_commit(h) == ESP_OK) && ok;
    nvs_close(h);
    if (ok) {
        ESP_LOGI(TAG, "credentials erased — rebooting into provisioning");
        vTaskDelay(pdMS_TO_TICKS(150));
        esp_restart();
    }
    return ok;
}

/* ================================================================== */
/*  DNS hijack (captive portal redirect)                              */
/* ================================================================== */

static void dns_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        ESP_LOGE(TAG, "dns: socket failed");
        s_dns_running = false;
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(DNS_PORT);
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        ESP_LOGE(TAG, "dns: bind failed");
        close(sock);
        s_dns_running = false;
        vTaskDelete(NULL);
        return;
    }

    /* 5 s idle timeout keeps the task responsive to deletion. */
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    (void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    ESP_LOGI(TAG, "dns hijack listening on :%d -> %s", DNS_PORT, PROV_AP_IP);

    uint8_t pkt[DNS_MAX_PACKET];
    while (s_dns_running) {
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        int n = recvfrom(sock, pkt, sizeof(pkt) - 16, 0,
                         (struct sockaddr *)&src, &slen);
        if (n < 12) {
            continue;              /* timeout or garbage — try again     */
        }
        /* Walk the QNAME of the FIRST question to find where it ends. */
        int q = 12;
        while (q < n && pkt[q] != 0) {
            q += pkt[q] + 1;
        }
        q += 1 + 4;                       /* root label + QTYPE + QCLASS */
        if (q > n) {
            continue;                     /* malformed query            */
        }
        uint16_t qtype = (uint16_t)((pkt[q - 4] << 8) | pkt[q - 3]);

        uint8_t reply[DNS_MAX_PACKET];
        if ((size_t)q + 16 > sizeof(reply)) {
            continue;
        }
        memcpy(reply, pkt, (size_t)q);    /* header + question only     */
        reply[2] = 0x85; reply[3] = 0x80; /* QR=1, AA=1, RD/RA, NOERROR */
        reply[4] = 0x00; reply[5] = 0x01; /* exactly one question       */
        reply[8] = reply[9] = reply[10] = reply[11] = 0x00; /* no NS/AR */
        size_t off = (size_t)q;
        if (qtype == 1u) {                /* A record: answer with AP IP */
            reply[6] = 0x00; reply[7] = 0x01;
            static const uint8_t ans[16] = {
                0xC0, 0x0C,               /* name = pointer to question */
                0x00, 0x01, 0x00, 0x01,   /* TYPE A, CLASS IN           */
                0x00, 0x00, 0x00, 0x3C,   /* TTL 60 s                   */
                0x00, 0x04,               /* RDLENGTH 4                 */
                192, 168, 4, 1
            };
            memcpy(reply + off, ans, sizeof(ans));
            off += sizeof(ans);
        } else {                          /* AAAA etc: empty NOERROR    */
            reply[6] = 0x00; reply[7] = 0x00;
        }
        (void)sendto(sock, reply, off, 0, (struct sockaddr *)&src, slen);
    }

    close(sock);
    vTaskDelete(NULL);
}

static void dns_start(void)
{
    if (s_dns_running) {
        return;
    }
    s_dns_running = true;
    if (xTaskCreate(dns_task, "dns_hijack", DNS_TASK_STACK, NULL,
                    4, NULL) != pdPASS) {
        s_dns_running = false;
    }
}

/* ================================================================== */
/*  Event handlers                                                    */
/* ================================================================== */

/* Reconnect from a one-shot timer. The old code slept (up to 10 s) INSIDE the
 * system event handler, which stalls the whole default event loop: IP and
 * WiFi events queued behind it, so recovery looked "stuck". */
static esp_timer_handle_t s_reconnect_timer;
static bool s_ever_got_ip = false;       /* connected at least once this boot */

static void reconnect_cb(void *arg)
{
    (void)arg;
    esp_wifi_connect();
}

static void schedule_reconnect(uint32_t delay_s)
{
    if (s_reconnect_timer == NULL) {
        const esp_timer_create_args_t a = {
            .callback = reconnect_cb, .name = "wifi_reconn",
        };
        if (esp_timer_create(&a, &s_reconnect_timer) != ESP_OK) {
            esp_wifi_connect();          /* no timer: retry immediately */
            return;
        }
    }
    (void)esp_timer_stop(s_reconnect_timer);
    (void)esp_timer_start_once(s_reconnect_timer, (uint64_t)delay_s * 1000000ull);
}

static void start_fallback_ap(void)
{
    esp_netif_create_default_wifi_ap();
    wifi_config_t wc = { 0 };
    strncpy((char *)wc.ap.ssid, PROV_SSID, sizeof(wc.ap.ssid) - 1u);
    strncpy((char *)wc.ap.password, PROV_PASS, sizeof(wc.ap.password) - 1u);
    wc.ap.ssid_len = (uint8_t)strlen(PROV_SSID);
    wc.ap.channel = PROV_CHANNEL;
    wc.ap.max_connection = PROV_MAX_CONN;
    wc.ap.authmode = WIFI_AUTH_WPA2_PSK;
    if (esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK
        || esp_wifi_set_config(WIFI_IF_AP, &wc) != ESP_OK) {
        ESP_LOGE(TAG, "could not open fallback AP");
        return;
    }
    s_ap_fallback = true;
    if (s_lock != NULL) { xSemaphoreTake(s_lock, portMAX_DELAY); }
    snprintf(s_ip, sizeof(s_ip), "%s", PROV_AP_IP);
    if (s_lock != NULL) { xSemaphoreGive(s_lock); }
    dns_start();
    set_state(WIFI_MGR_PROVISIONING);
    dashboard_data_note_wifi(DASH_WIFI_PROVISIONING, PROV_SSID, PROV_AP_IP, 0);
    ESP_LOGW(TAG, "Setup AP \"%s\" (pw \"%s\") open — http://%s/",
             PROV_SSID, PROV_PASS, PROV_AP_IP);
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id,
                          void *event_data)
{
    (void)arg;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "STA started — connecting");
        esp_wifi_connect();
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d =
            (const wifi_event_sta_disconnected_t *)event_data;
        s_sta_retries++;
        ESP_LOGW(TAG, "STA disconnected, reason %d (%s) — attempt %d",
                 d ? (int)d->reason : -1,
                 (d && (d->reason == WIFI_REASON_AUTH_FAIL
                        || d->reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT
                        || d->reason == WIFI_REASON_HANDSHAKE_TIMEOUT))
                     ? "wrong password?" :
                 (d && d->reason == WIFI_REASON_NO_AP_FOUND)
                     ? "SSID not found / 5 GHz only?" : "see esp_wifi docs",
                 s_sta_retries);

        if (!s_ever_got_ip && !s_ap_fallback
            && s_sta_retries > STA_MAX_RETRIES) {
            /* Credentials never worked this boot. Do NOT erase them (a
             * typo or a router that is merely off would wipe good
             * settings and loop forever). Open the setup AP *beside* the
             * station so the user can correct them from the dashboard. */
            logger_logf(LOG_LVL_ERR, "wifi",
                        "STA failed %d times — setup AP opened",
                        STA_MAX_RETRIES);
            start_fallback_ap();
        }
        /* Keep trying forever; back off 1..10 s. */
        uint32_t delay_s = (uint32_t)s_sta_retries;
        if (delay_s > 10u) {
            delay_s = 10u;
        }
        schedule_reconnect(delay_s);
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)event_data;
        if (s_lock != NULL) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
        }
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&evt->ip_info.ip));
        if (s_lock != NULL) {
            xSemaphoreGive(s_lock);
        }
        s_sta_retries = 0;
        s_ever_got_ip = true;
        if (s_ap_fallback) {
            s_ap_fallback = false;
            s_dns_running = false;          /* dns task exits by itself */
            (void)esp_wifi_set_mode(WIFI_MODE_STA);
        }
        set_state(WIFI_MGR_CONNECTED);
        ESP_LOGI(TAG, "got IP %s", s_ip);
        logger_logf(LOG_LVL_INFO, "wifi", "connected, ip %s", s_ip);
        char empty[1] = { '\0' };
        dashboard_data_note_wifi(DASH_WIFI_CONNECTED, s_ssid, s_ip, 0);
        (void)empty;
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        /* RSSI updates come from the poller (task_wifi). */
    }
}

/* ================================================================== */
/*  Init / mode bring-up                                              */
/* ================================================================== */

static void start_station(const char *ssid, const char *pass)
{
    strncpy(s_ssid, ssid, sizeof(s_ssid) - 1u);
    s_ssid[sizeof(s_ssid) - 1u] = '\0';

    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (s_sta_netif != NULL) {
        (void)esp_netif_set_hostname(s_sta_netif, "health-monitor");
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                               &event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                               &event_handler, NULL);

    wifi_config_t wc = { 0 };
    strncpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid) - 1u);
    strncpy((char *)wc.sta.password, pass, sizeof(wc.sta.password) - 1u);
    /* Accept whatever the router offers (open, WPA2, WPA3 transition).
     * The old WPA_WPA2 threshold silently rejected open and WPA3-only
     * networks, and the first-match scan could lock onto a weak AP. */
    wc.sta.threshold.authmode = (pass[0] == '\0') ? WIFI_AUTH_OPEN
                                                   : WIFI_AUTH_WPA2_PSK;
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    wc.sta.pmf_cfg.capable = true;
    wc.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* Modem power-save makes the radio sleep between beacons: WebSocket
     * pushes and HTTP polls then arrive late or time out, which shows up as
     * a dashboard that "disconnects" at random. This is a mains/bench
     * instrument, so keep the radio awake. */
    (void)esp_wifi_set_ps(WIFI_PS_NONE);

    set_state(WIFI_MGR_CONNECTING);
    dashboard_data_note_wifi(DASH_WIFI_CONNECTING, s_ssid, "", 0);
}

static void start_provisioning(void)
{
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wc = { 0 };
    strncpy((char *)wc.ap.ssid, PROV_SSID, sizeof(wc.ap.ssid) - 1u);
    strncpy((char *)wc.ap.password, PROV_PASS, sizeof(wc.ap.password) - 1u);
    wc.ap.channel = PROV_CHANNEL;
    wc.ap.max_connection = PROV_MAX_CONN;
    wc.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    snprintf(s_ip, sizeof(s_ip), "%s", PROV_AP_IP);
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
    s_ssid[0] = '\0';

    dns_start();
    set_state(WIFI_MGR_PROVISIONING);
    logger_logf(LOG_LVL_WARN, "wifi", "provisioning AP up (%s)",
                PROV_SSID);
    dashboard_data_note_wifi(DASH_WIFI_PROVISIONING, PROV_SSID, PROV_AP_IP, 0);

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "=== PROVISIONING MODE ===");
    ESP_LOGI(TAG, "Join WiFi \"%s\" (password \"%s\")", PROV_SSID, PROV_PASS);
    ESP_LOGI(TAG, "Open http://%s/ and enter your WiFi credentials.",
             PROV_AP_IP);
    ESP_LOGI(TAG, "");
}

void wifi_manager_init(void)
{
    s_lock = xSemaphoreCreateMutex();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    char ssid[33] = { 0 };
    char pass[65] = { 0 };
    if (read_credentials(ssid, sizeof(ssid), pass, sizeof(pass))) {
        ESP_LOGI(TAG, "stored credentials for \"%s\" — station mode", ssid);
        start_station(ssid, pass);
    } else {
        ESP_LOGW(TAG, "no stored credentials — provisioning AP");
        start_provisioning();
    }
}

void wifi_manager_poll_rssi(void)
{
    if (s_state != WIFI_MGR_CONNECTED) {
        s_rssi = 0;
        return;
    }
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        s_rssi = (int)ap.rssi;
    }
}
