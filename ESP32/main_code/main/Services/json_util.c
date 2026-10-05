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

/* ================================================================== */
/*  Request-body scanners                                             */
/* ================================================================== */

#include <stdlib.h>
#include <ctype.h>

/* Locate "key" followed by ':' — returns pointer to the char after the
 * colon, or NULL. Tolerates whitespace between tokens. */
static const char *find_value(const char *json, const char *key)
{
    if (json == NULL || key == NULL) {
        return NULL;
    }
    size_t klen = strlen(key);
    const char *p = json;
    while ((p = strstr(p, key)) != NULL) {
        /* Must be preceded by a quote and followed by a quote + colon. */
        if (p > json && p[-1] == '"'
            && p[klen] == '"') {
            const char *q = p + klen + 1;
            while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') {
                q++;
            }
            if (*q == ':') {
                q++;
                while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') {
                    q++;
                }
                return q;
            }
        }
        p += klen;
    }
    return NULL;
}

int json_find_long(const char *json, const char *key, long long *out)
{
    const char *v = find_value(json, key);
    if (v == NULL || *v == '\0') {
        return -1;
    }
    char *end = NULL;
    long long val = strtoll(v, &end, 10);
    if (end == v) {
        return -1;
    }
    if (out != NULL) {
        *out = val;
    }
    return 0;
}

int json_find_double(const char *json, const char *key, double *out)
{
    const char *v = find_value(json, key);
    if (v == NULL || *v == '\0') {
        return -1;
    }
    char *end = NULL;
    double val = strtod(v, &end);
    if (end == v) {
        return -1;
    }
    if (out != NULL) {
        *out = val;
    }
    return 0;
}

int json_find_str(const char *json, const char *key, char *out, size_t cap)
{
    const char *v = find_value(json, key);
    if (v == NULL || *v != '"') {
        return -1;
    }
    v++;
    size_t n = 0;
    while (*v != '"') {
        if (*v == '\0') {
            return -1;
        }
        char c = *v;
        if (c == '\\' && v[1] != '\0') {
            v++;
            switch (*v) {
                case 'n':  c = '\n'; break;
                case 'r':  c = '\r'; break;
                case 't':  c = '\t'; break;
                case '"':  c = '"';  break;
                case '\\': c = '\\'; break;
                default:   c = *v;  break;
            }
        }
        if (n + 1u >= cap) {
            return -2;
        }
        out[n++] = c;
        v++;
    }
    out[n] = '\0';
    return 0;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    return -1;
}

int urlform_find(const char *body, const char *key, char *out, size_t cap)
{
    if (body == NULL || key == NULL || out == NULL || cap == 0u) {
        return -1;
    }
    size_t klen = strlen(key);
    const char *p = body;
    while ((p = strstr(p, key)) != NULL) {
        if ((p == body || p[-1] == '&' || p[-1] == '?')
            && p[klen] == '=') {
            const char *v = p + klen + 1;
            size_t n = 0u;
            while (*v != '\0' && *v != '&') {
                char c = *v;
                if (c == '+') {
                    c = ' ';
                } else if (c == '%' && hexval(v[1]) >= 0 && hexval(v[2]) >= 0) {
                    c = (char)((hexval(v[1]) << 4) | hexval(v[2]));
                    v += 2;
                }
                if (n + 1u >= cap) {
                    return -2;
                }
                out[n++] = c;
                v++;
            }
            out[n] = '\0';
            return 0;
        }
        p += klen;
    }
    return -1;
}
