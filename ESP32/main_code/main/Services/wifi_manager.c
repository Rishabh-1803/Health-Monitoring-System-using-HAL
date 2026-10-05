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

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_system.h"

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
#define DNS_TASK_STACK      3072
#define DNS_PORT            53
#define DNS_MAX_PACKET      512

static wifi_mgr_state_t s_state = WIFI_MGR_CONNECTING;
static char s_ip[16] = "";
static int  s_rssi = 0;
static char s_ssid[33] = "";
static SemaphoreHandle_t s_lock;

static int s_sta_retries = 0;
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
        vTaskDelay(pdMS_TO_TICKS(150));   /* let the HTTP reply escape */
        esp_restart();
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
        /* Answer: copy header, set QR/AA, one answer = AP IP. */
        uint8_t *r = pkt + n;      /* build the reply in the tail        */
        r[0] = pkt[0]; r[1] = pkt[1];
        r[2] = 0x85; r[3] = 0x80;            /* QR=1, AA=1, RCODE=0      */
        r[4] = pkt[4]; r[5] = pkt[5];        /* QDCOUNT (echoed)         */
        r[6] = 0x00; r[7] = 0x01;            /* ANCOUNT = 1              */
        r[8] = 0x00; r[9] = 0x00;
        r[10] = 0x00; r[11] = 0x00;

        /* The reply needs the question section copied after the header;
         * we move the question to sit right after our new header. */
        uint8_t reply[DNS_MAX_PACKET];
        memcpy(reply, r, 12);
        memcpy(reply + 12, pkt + 12, (size_t)(n - 12));
        size_t qlen = (size_t)(n - 12);
        size_t off = 12 + qlen;
        if (off + 16 <= sizeof(reply)) {
            /* Type A, class IN, ttl 60, rdlen 4, rdata = AP IP. */
            static const uint8_t tail[16] = {
                0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x3C,
                0x00, 0x04, 192, 168, 4, 1, 0x00, 0x00
            };
            /* tail[14] = A record rdlength trick: we only need the first
             * 14 bytes (ends with the 4 address bytes). */
            memcpy(reply + off, tail, 14);
            off += 14;
            (void)sendto(sock, reply, off, 0,
                         (struct sockaddr *)&src, slen);
        }
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

static void event_handler(void *arg, esp_event_base_t base, int32_t id,
                          void *event_data)
{
    (void)arg;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "STA started — connecting");
        esp_wifi_connect();
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_sta_retries++;
        if (s_sta_retries <= STA_MAX_RETRIES) {
            /* Backoff: 1s, 2s, ... capped at 10s. */
            uint32_t delay_s = (uint32_t)s_sta_retries;
            if (delay_s > 10u) {
                delay_s = 10u;
            }
            ESP_LOGW(TAG, "STA disconnected — retry %d/%d in %lu s",
                     s_sta_retries, STA_MAX_RETRIES,
                     (unsigned long)delay_s);
            vTaskDelay(pdMS_TO_TICKS(delay_s * 1000u));
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "STA gave up after %d retries — starting "
                     "provisioning AP", STA_MAX_RETRIES);
            /* Reboot into provisioning mode: the cleanest mode switch,
             * because netif/wifi re-init on the AP side is exactly the
             * same path as a fresh boot without credentials. */
            (void)wifi_manager_clear_credentials();   /* reboots */
        }
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
        set_state(WIFI_MGR_CONNECTED);
        ESP_LOGI(TAG, "got IP %s", s_ip);
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

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                               &event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                               &event_handler, NULL);

    wifi_config_t wc = { 0 };
    strncpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid) - 1u);
    strncpy((char *)wc.sta.password, pass, sizeof(wc.sta.password) - 1u);
    wc.sta.threshold.authmode = WIFI_AUTH_WPA_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

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
