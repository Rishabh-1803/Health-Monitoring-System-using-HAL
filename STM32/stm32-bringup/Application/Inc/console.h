/**
 * @file    console.h
 * @brief   Line-oriented console over the USB CDC link.
 *
 * Output goes straight out through CDC_Transmit_FS, retrying while the
 * endpoint is busy. Input arrives in USB interrupt context, so the ISR
 * only drops bytes into a ring buffer and the CLI task drains it.
 *
 * The write path assumes the FreeRTOS scheduler is running: it yields
 * with vTaskDelay while USB is busy, so no print may happen before
 * osKernelStart(). console_init() is the exception and must be called
 * before the scheduler starts, so the receive ring buffer exists by the
 * time the USB interrupt can push into it.
 */

#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdbool.h>
#include <stdint.h>

/** Longest line the CLI will accept, including the terminating NUL. */
#define CONSOLE_LINE_MAX   64u

/** Set up the ring buffer and the transmit mutex. Call before any print. */
void console_init(void);

/**
 * True once the host has enumerated the CDC device and it is safe to
 * print. Before this point CDC_Transmit_FS would dereference a NULL
 * class-data pointer, so the bring-up task waits on this.
 */
bool console_is_ready(void);

/* ------------------------------------------------------------------ */
/*  Output                                                            */
/* ------------------------------------------------------------------ */

/** Write a NUL-terminated string. Returns false if USB never accepted it. */
bool console_print(const char *s);

/** Same, with a trailing CRLF (terminals on Windows want both). */
bool console_println(const char *s);

/**
 * printf-style output. Formatted into a small task-local buffer, so keep
 * the result under ~200 characters; longer output is truncated rather
 * than overflowing anything.
 */
bool console_printf(const char *fmt, ...);

/* ------------------------------------------------------------------ */
/*  Input                                                             */
/* ------------------------------------------------------------------ */

/**
 * Called from the USB CDC receive callback, i.e. in interrupt context.
 * Copies bytes into the ring buffer and returns immediately. Bytes that
 * arrive when the buffer is full are dropped, which is the right
 * trade-off for a human-typed console.
 */
void console_rx_push_from_isr(const uint8_t *data, uint32_t len);

/**
 * Pull one byte from the ring buffer.
 * Returns false if nothing is waiting -- it never blocks.
 */
bool console_getchar(uint8_t *out);

/**
 * Collect characters until Enter, echoing as it goes and honouring
 * backspace. Yields between polls so other tasks keep running.
 *
 * Echo is batched into one transfer per poll, not one per keystroke, so a
 * pasted line does not turn into dozens of separate USB writes.
 *
 * Ctrl-C returns true with an empty buffer -- treat it as "the user
 * changed their mind", not as an error.
 *
 * @param buf        destination, always NUL-terminated on true
 * @param buf_len    size of buf, clamped to CONSOLE_LINE_MAX
 * @param timeout_ms 0 waits forever
 * @return true if a complete line was read, false on timeout
 */
bool console_readline(char *buf, uint32_t buf_len, uint32_t timeout_ms);

/**
 * True if a key is waiting. The long-running tests poll this so any
 * keypress can abort them.
 */
bool console_key_pressed(void);

/** Throw away anything buffered -- used before showing a fresh prompt. */
void console_flush_rx(void);

#endif /* CONSOLE_H */
