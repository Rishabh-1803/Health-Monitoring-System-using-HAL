/**
 * @file    json_util.c
 * @brief   Bounded JSON writer implementation (pure C, host-testable).
 */

#include "json_util.h"

#include <stdio.h>
#include <string.h>

static void put(jsonw_t *w, char c)
{
    if (w->len + 1u < w->cap) {         /* keep one byte for the NUL     */
        w->buf[w->len] = c;
    }
    w->len++;                           /* always counts, even truncated */
    if (w->len < w->cap) {
        w->buf[w->len] = '\0';
    }
}

void jsonw_init(jsonw_t *w, char *buf, size_t cap)
{
    if (w == NULL || buf == NULL || cap == 0u) {
        return;
    }
    w->buf = buf;
    w->cap = cap;
    w->len = 0u;
    buf[0] = '\0';
}

size_t jsonw_len(const jsonw_t *w)
{
    return (w == NULL) ? 0u : w->len;
}

void jsonw_raw(jsonw_t *w, const char *s)
{
    if (w == NULL || s == NULL) {
        return;
    }
    while (*s != '\0') {
        put(w, *s++);
    }
}

void jsonw_str(jsonw_t *w, const char *s)
{
    if (w == NULL) {
        return;
    }
    put(w, '"');
    if (s != NULL) {
        for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; p++) {
            unsigned char c = *p;
            switch (c) {
                case '"':  jsonw_raw(w, "\\\""); break;
                case '\\': jsonw_raw(w, "\\\\"); break;
                case '\n': jsonw_raw(w, "\\n");  break;
                case '\r': jsonw_raw(w, "\\r");  break;
                case '\t': jsonw_raw(w, "\\t");  break;
                default:
                    if (c < 0x20u) {
                        char esc[8];
                        (void)snprintf(esc, sizeof(esc), "\\u%04x", (unsigned)c);
                        jsonw_raw(w, esc);
                    } else {
                        put(w, (char)c);
                    }
                    break;
            }
        }
    }
    put(w, '"');
}

void jsonw_num(jsonw_t *w, double v)
{
    if (w == NULL) {
        return;
    }
    char tmp[32];
    (void)snprintf(tmp, sizeof(tmp), "%.2f", v);
    jsonw_raw(w, tmp);
}

void jsonw_int(jsonw_t *w, long long v)
{
    if (w == NULL) {
        return;
    }
    char tmp[32];
    (void)snprintf(tmp, sizeof(tmp), "%lld", v);
    jsonw_raw(w, tmp);
}

void jsonw_bool(jsonw_t *w, int v)
{
    jsonw_raw(w, v ? "true" : "false");
}
