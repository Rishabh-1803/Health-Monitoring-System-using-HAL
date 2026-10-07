/**
 * @file    littlefs_storage.c
 * @brief   LittleFS-backed storage (optional, fail-safe).
 */

#include "littlefs_storage.h"
#include "project_config.h"

#if HMS_USE_LITTLEFS

#include "esp_littlefs.h"
#include "esp_log.h"
#include "json_util.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "LFS";

#define PARTITION_LABEL   "littlefs"
#define HIST_PATH         "/lfs/hist0.csv"
#define HIST_PATH_OLD     "/lfs/hist1.csv"
#define LOG_PATH          "/lfs/log.txt"
#define CFG_PATH          "/lfs/config.json"
#define HIST_ROTATE_BYTES (256 * 1024)
#define LOG_TRIM_BYTES    (128 * 1024)

static bool s_ok = false;

void littlefs_storage_mount(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .partition_label = PARTITION_LABEL,
        .base_path = "/lfs",
        .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mount failed (0x%x) — running RAM-only", err);
        s_ok = false;
        return;
    }
    s_ok = true;
    size_t total = 0, used = 0;
    if (esp_littlefs_info(PARTITION_LABEL, &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "mounted: %u/%u KB used",
                 (unsigned)(used / 1024), (unsigned)(total / 1024));
    }
}

bool littlefs_storage_ok(void)
{
    return s_ok;
}

/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */

static void rotate_if_full(const char *path, const char *old_path,
                           size_t limit)
{
    long sz = 0;
    FILE *f = fopen(path, "rb");
    if (f != NULL) {
        (void)fseek(f, 0, SEEK_END);
        sz = ftell(f);
        fclose(f);
    }
    if (sz < 0 || (size_t)sz < limit) {
        return;
    }
    remove(old_path);
    (void)rename(path, old_path);
    ESP_LOGI(TAG, "rotated %s -> %s", path, old_path);
}

void littlefs_storage_append_history(uint32_t t_ms, const float v[3])
{
    if (!s_ok || v == NULL) {
        return;
    }
    rotate_if_full(HIST_PATH, HIST_PATH_OLD, HIST_ROTATE_BYTES);
    FILE *f = fopen(HIST_PATH, "a");
    if (f == NULL) {
        return;
    }
    fprintf(f, "%lu,%.2f,%.3f,%.3f\n",
            (unsigned long)t_ms, (double)v[0], (double)v[1], (double)v[2]);
    fclose(f);
}

void littlefs_storage_append_log(uint32_t t_ms, uint8_t level,
                                 const char *tag, const char *text)
{
    if (!s_ok) {
        return;
    }
    long sz = 0;
    FILE *f = fopen(LOG_PATH, "rb");
    if (f != NULL) {
        (void)fseek(f, 0, SEEK_END);
        sz = ftell(f);
        fclose(f);
    }
    if (sz > LOG_TRIM_BYTES) {
        /* Trim the front half by rewriting (simple + bounded). */
        f = fopen(LOG_PATH, "r");
        if (f != NULL) {
            (void)fseek(f, LOG_TRIM_BYTES / 2, SEEK_SET);
            char chunk[512];
            size_t n;
            FILE *nf = fopen(LOG_PATH ".tmp", "w");
            while (nf != NULL && (n = fread(chunk, 1, sizeof(chunk), f)) > 0u) {
                fwrite(chunk, 1, n, nf);
            }
            if (nf != NULL) {
                fclose(nf);
                remove(LOG_PATH);
                (void)rename(LOG_PATH ".tmp", LOG_PATH);
            }
            fclose(f);
        }
    }

    f = fopen(LOG_PATH, "a");
    if (f == NULL) {
        return;
    }
    fprintf(f, "%lu.%03lu,%u,%s,%s\n",
            (unsigned long)(t_ms / 1000u), (unsigned long)(t_ms % 1000u),
            (unsigned)level, (tag != NULL) ? tag : "app",
            (text != NULL) ? text : "");
    fclose(f);
}

#else /* !HMS_USE_LITTLEFS */

void littlefs_storage_mount(void) { }
bool littlefs_storage_ok(void) { return false; }
void littlefs_storage_append_history(uint32_t t_ms, const float v[3])
{ (void)t_ms; (void)v; }
void littlefs_storage_append_log(uint32_t t_ms, uint8_t level,
                                 const char *tag, const char *text)
{ (void)t_ms; (void)level; (void)tag; (void)text; }

#endif /* HMS_USE_LITTLEFS */

/* ------------------------------------------------------------------ */
/*  Config (thresholds + sample rate): always stored in NVS            */
/*  so it survives a reset even when LittleFS is disabled.             */
/* ------------------------------------------------------------------ */

#include "nvs.h"
#include <math.h>

#define CFG_NS   "hms_cfg"
#define CFG_KEY  "cfg"

typedef struct {
    float    thr[3];
    uint16_t rate;
    uint16_t magic;
} stored_cfg_t;

#define CFG_MAGIC 0xC0F1u

bool littlefs_storage_save_config(const float thr[3], uint16_t rate_ms)
{
    if (thr == NULL) {
        return false;
    }
    stored_cfg_t c = { { thr[0], thr[1], thr[2] }, rate_ms, CFG_MAGIC };
    nvs_handle_t h;
    if (nvs_open(CFG_NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    bool ok = (nvs_set_blob(h, CFG_KEY, &c, sizeof(c)) == ESP_OK)
              && (nvs_commit(h) == ESP_OK);
    nvs_close(h);
    return ok;
}

bool littlefs_storage_load_config(float thr_out[3], uint16_t *rate_out)
{
    nvs_handle_t h;
    if (thr_out == NULL || nvs_open(CFG_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    stored_cfg_t c;
    size_t len = sizeof(c);
    esp_err_t err = nvs_get_blob(h, CFG_KEY, &c, &len);
    nvs_close(h);
    if (err != ESP_OK || len != sizeof(c) || c.magic != CFG_MAGIC) {
        return false;
    }
    for (int i = 0; i < 3; i++) {
        if (!isfinite(c.thr[i])) {
            return false;
        }
        thr_out[i] = c.thr[i];
    }
    if (rate_out != NULL) {
        *rate_out = (c.rate >= 50u && c.rate <= 1000u) ? c.rate : 200u;
    }
    return true;
}
