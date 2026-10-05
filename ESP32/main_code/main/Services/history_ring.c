/**
 * @file    history_ring.c
 * @brief   Ring buffer implementation (pure C, host-testable).
 */

#include "history_ring.h"

#include <string.h>

void history_ring_init(history_ring_t *r, hr_sample_t *storage, uint16_t cap)
{
    if (r == NULL || storage == NULL || cap == 0u) {
        return;
    }
    r->buf = storage;
    r->cap = cap;
    r->head = 0u;
    r->count = 0u;
}

void history_ring_push(history_ring_t *r, uint32_t t_ms, const float *vals)
{
    if (r == NULL || r->buf == NULL) {
        return;
    }
    r->buf[r->head].t_ms = t_ms;
    if (vals != NULL) {
        for (int i = 0; i < HR_CHANNELS; i++) {
            r->buf[r->head].v[i] = vals[i];
        }
    }
    r->head = (uint16_t)((r->head + 1u) % r->cap);
    if (r->count < r->cap) {
        r->count++;
    }
}

uint16_t history_ring_count(const history_ring_t *r)
{
    if (r == NULL) {
        return 0u;
    }
    return r->count;
}

const hr_sample_t *history_ring_at(const history_ring_t *r, uint16_t idx)
{
    if (r == NULL || r->buf == NULL || idx >= r->count) {
        return NULL;
    }
    /* Oldest sample sits right after the write slot once the ring is full. */
    uint16_t start = (uint16_t)((r->head + r->cap - r->count) % r->cap);
    uint16_t slot = (uint16_t)((start + idx) % r->cap);
    return &r->buf[slot];
}

uint16_t history_ring_copy_since(const history_ring_t *r, uint32_t since_ms,
                                 hr_sample_t *out, uint16_t max)
{
    if (r == NULL || out == NULL) {
        return 0u;
    }
    uint16_t copied = 0u;
    for (uint16_t i = 0; i < r->count && copied < max; i++) {
        const hr_sample_t *s = history_ring_at(r, i);
        if (s == NULL) {
            break;
        }
        if (s->t_ms >= since_ms) {
            out[copied++] = *s;
        }
    }
    return copied;
}
