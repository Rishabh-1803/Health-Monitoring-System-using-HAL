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

bool littlefs_storage_save_config(const float thr[3], uint16_t rate_ms)
{
    if (!s_ok || thr == NULL) {
        return false;
    }
    FILE *f = fopen(CFG_PATH, "w");
    if (f == NULL) {
        return false;
    }
    fprintf(f, "{\"thr_t\":%.2f,\"thr_c\":%.2f,\"thr_v\":%.2f,\"rate\":%u}\n",
            (double)thr[0], (double)thr[1], (double)thr[2],
            (unsigned)rate_ms);
    fclose(f);
    return true;
}

bool littlefs_storage_load_config(float thr_out[3], uint16_t *rate_out)
{
    if (!s_ok) {
        return false;
    }
    FILE *f = fopen(CFG_PATH, "r");
    if (f == NULL) {
        return false;
    }
    char buf[128] = { 0 };
    size_t n = fread(buf, 1, sizeof(buf) - 1u, f);
    fclose(f);
    buf[n] = '\0';

    /* Reuse the fixed-key scanners from json_util (pure C). */
    double t = 0, c = 0, v = 0;
    long long rate = 200;
    if (json_find_double(buf, "thr_t", &t) != 0
        || json_find_double(buf, "thr_c", &c) != 0
        || json_find_double(buf, "thr_v", &v) != 0
        || json_find_long(buf, "rate", &rate) != 0) {
        return false;
    }
    thr_out[0] = (float)t;
    thr_out[1] = (float)c;
    thr_out[2] = (float)v;
    if (rate_out != NULL) {
        *rate_out = (rate >= 50 && rate <= 1000) ? (uint16_t)rate : 200u;
    }
    return true;
}

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
bool littlefs_storage_save_config(const float thr[3], uint16_t rate_ms)
{ (void)thr; (void)rate_ms; return false; }
bool littlefs_storage_load_config(float thr_out[3], uint16_t *rate_out)
{ (void)thr_out; (void)rate_out; return false; }
void littlefs_storage_append_history(uint32_t t_ms, const float v[3])
{ (void)t_ms; (void)v; }
void littlefs_storage_append_log(uint32_t t_ms, uint8_t level,
                                 const char *tag, const char *text)
{ (void)t_ms; (void)level; (void)tag; (void)text; }

#endif /* HMS_USE_LITTLEFS */
