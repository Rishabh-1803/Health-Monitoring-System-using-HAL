/**
 * @file    json_util.h
 * @brief   Tiny bounded JSON writer + escaping (no cJSON dependency).
 *
 * Pure C: compiles on the ESP32 and in the host unit tests. Every writer
 * truncates safely at the buffer end rather than overflowing, and always
 * NUL-terminates what it managed to write.
 */

#ifndef JSON_UTIL_H
#define JSON_UTIL_H

#include <stddef.h>
#include <stdint.h>

/** Bounded string builder state. */
typedef struct {
    char  *buf;
    size_t cap;     /* total capacity, including the NUL slot            */
    size_t len;     /* bytes written, excluding the NUL                   */
} jsonw_t;

/** Start writing into buf (cap must include room for the NUL). */
void jsonw_init(jsonw_t *w, char *buf, size_t cap);

/** Bytes written so far (excluding NUL). */
size_t jsonw_len(const jsonw_t *w);

/** Append raw bytes verbatim (for commas, braces, numbers you built). */
void jsonw_raw(jsonw_t *w, const char *s);

/** Append a quoted, escaped string. */
void jsonw_str(jsonw_t *w, const char *s);

/** Append a 2-decimal float ("24.50"). Safe with -Wformat for doubles. */
void jsonw_num(jsonw_t *w, double v);

/** Like jsonw_num with a chosen number of decimals; NaN/inf become 0. */
void jsonw_numf(jsonw_t *w, double v, int decimals);

/** Append an integer. */
void jsonw_int(jsonw_t *w, long long v);

/** Append true/false. */
void jsonw_bool(jsonw_t *w, int v);


#endif /* JSON_UTIL_H */

/* ================================================================== */
/*  Tiny request-body scanners (sufficient for our fixed POST bodies)  */
/* ================================================================== */

/** Find "key":<integer>. Returns 0 and writes *out on success, -1 if absent. */
int json_find_long(const char *json, const char *key, long long *out);

/** Find "key":<number> (float). Returns 0 and writes *out, -1 if absent. */
int json_find_double(const char *json, const char *key, double *out);

/**
 * Find "key":"string". Copies (unescaped) into out. Returns 0 on success,
 * -1 if absent, -2 if the destination is too small.
 */
int json_find_str(const char *json, const char *key, char *out, size_t cap);

/** URL-decode "a=b&c=d" bodies: find key, decode into out. -1 absent. */
int urlform_find(const char *body, const char *key, char *out, size_t cap);
