/**
 * @file    littlefs_storage.h
 * @brief   Flash storage: history CSV + incident log + config.
 *
 * Everything degrades gracefully: when HMS_USE_LITTLEFS is 0 (or the
 * mount fails), every call is a harmless no-op and the system runs
 * from RAM + NVS. Flash writes are sparse by design —
 *   history: one line per minute (rotates at 256 KB, keeps 2 files)
 *   logs:    flushed on alarm and every 5 minutes
 *   config:  written only when the user changes a setting
 * so the flash wears at a rate a bench/instrument device can live
 * with for years.
 */

#ifndef LITTLEFS_STORAGE_H
#define LITTLEFS_STORAGE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/** Mount the "littlefs" partition. Call once at boot. */
void littlefs_storage_mount(void);

/** True when the filesystem is mounted and writable. */
bool littlefs_storage_ok(void);

/* ---- config (thresholds + sample rate) --------------------------- */

bool littlefs_storage_save_config(const float thr[3], uint16_t rate_ms);

/**
 * Load the stored config into the out-parameters.
 * @return true when a stored config was applied.
 */
bool littlefs_storage_load_config(float thr_out[3], uint16_t *rate_out);

/* ---- history CSV (t_ms,temp,cur,vib) ------------------------------ */

void littlefs_storage_append_history(uint32_t t_ms, const float v[3]);

/* ---- incident log -------------------------------------------------- */

void littlefs_storage_append_log(uint32_t t_ms, uint8_t level,
                                 const char *tag, const char *text);

#endif /* LITTLEFS_STORAGE_H */
