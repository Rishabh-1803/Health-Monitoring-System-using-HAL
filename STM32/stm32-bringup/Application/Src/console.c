/**
 * @file    console.c
 * @brief   USB CDC console: blocking writes, interrupt-fed ring buffer reads.
 *
 * Three details are worth knowing before changing anything here.
 *
 * 1. CDC_Transmit_FS dereferences hUsbDeviceFS.pClassData without checking
 *    it. Called before MX_USB_DEVICE_Init() has run, that is a NULL
 *    dereference and an instant hard fault. So every write first checks
 *    that the device has reached the CONFIGURED state. That check is also
 *    what lets the bring-up task simply wait for the host to enumerate.
 *
 * 2. CDC_Transmit_FS does NOT copy the caller's data. It stores the
 *    pointer (USBD_CDC_SetTxBuffer) and the bytes are read out of that
 *    buffer later, from the USB interrupt, by PCD_WriteEmptyTxFifo. Hand
 *    it a stack local and the frame is very likely gone by then, so the
 *    host receives whatever the next call left on the stack. Every write
 *    on this path is therefore copied into a static staging buffer first,
 *    and the buffer is only overwritten once the previous packet has been
 *    fully handed over (TxState back to 0).
 *
 * 3. Writes are chunked to 48 bytes. A bulk transfer whose length is an
 *    exact multiple of the 64-byte endpoint size needs a zero-length
 *    packet to terminate it, and the ST stack is unreliable about that.
 *    48 is never a multiple of 64, so the situation cannot arise.
 *
 * Nothing here uses %f. Floating-point printf costs several kB of newlib
 * and needs an extra linker flag; the tests format fractions by hand from
 * integers instead.
 */

#include "console.h"

#include "usbd_cdc_if.h"      /* CDC_Transmit_FS                */
#include "usbd_def.h"         /* USBD_OK, USBD_STATE_CONFIGURED */

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* usb_device.h declares MX_USB_DEVICE_Init but not the handle itself --
 * the handle is only defined in usb_device.c. ST's own generated
 * usbd_cdc_if.c reaches it with exactly this extern, so do the same
 * rather than editing a CubeMX-owned header. */
extern USBD_HandleTypeDef hUsbDeviceFS;

/* ================================================================== */
/*  Receive ring buffer                                               */
/* ================================================================== */

/* Power of two so the wrap is a mask, not a modulo. One writer (the USB
 * ISR) and one reader (the CLI task), so no locking is required as long
 * as head and tail are each touched by only one side. */
#define RX_BUF_SIZE   128u
#define RX_BUF_MASK   (RX_BUF_SIZE - 1u)

static volatile uint8_t  s_rx[RX_BUF_SIZE];
static volatile uint32_t s_rx_head;    /* written by the ISR  */
static volatile uint32_t s_rx_tail;    /* written by the task */

static SemaphoreHandle_t s_tx_mutex;

/* Terminals variously send CR, LF or CRLF for Enter. The first one seen
 * terminates the line; this remembers which, so the other half of a CRLF
 * pair is discarded at the start of the next call instead of surfacing as
 * a spurious empty line and a doubled prompt. Touched only by the reader. */
static uint8_t s_last_eol;

void console_init(void)
{
    s_rx_head  = 0u;
    s_rx_tail  = 0u;
    s_last_eol = 0u;
    s_tx_mutex = xSemaphoreCreateMutex();
}

void console_rx_push_from_isr(const uint8_t *data, uint32_t len)
{
    if (data == NULL) {
        return;
    }
    for (uint32_t i = 0u; i < len; i++) {
        uint32_t next = (s_rx_head + 1u) & RX_BUF_MASK;
        if (next == s_rx_tail) {
            break;              /* full: drop the rest, a human can retype */
        }
        s_rx[s_rx_head] = data[i];
        s_rx_head = next;
    }
}

bool console_getchar(uint8_t *out)
{
    if (s_rx_tail == s_rx_head) {
        return false;
    }
    *out = s_rx[s_rx_tail];
    s_rx_tail = (s_rx_tail + 1u) & RX_BUF_MASK;
    return true;
}

bool console_key_pressed(void)
{
    return (s_rx_tail != s_rx_head);
}

void console_flush_rx(void)
{
    s_rx_tail = s_rx_head;
    s_last_eol = 0u;
}

/* ================================================================== */
/*  Transmit                                                          */
/* ================================================================== */

/* Chunk size. Deliberately not a multiple of the 64-byte endpoint size,
 * so a transfer never needs a terminating zero-length packet. */
#define TX_CHUNK   48u

/* The staging buffer the USB stack actually reads from. See note 2 in the
 * file header: CDC_Transmit_FS keeps the pointer rather than the bytes.
 * Static, so it outlives every caller's stack frame. Guarded by the
 * transmit mutex plus the TxState==0 wait in tx_chunk, so only one packet
 * is ever in flight out of it. */
static uint8_t s_tx_stage[TX_CHUNK];

static bool usb_ready(void)
{
    return (hUsbDeviceFS.dev_state == USBD_STATE_CONFIGURED);
}

bool console_is_ready(void)
{
    return usb_ready();
}

/** True once the stack has finished with the previous packet. */
static bool tx_idle(void)
{
    const USBD_CDC_HandleTypeDef *hcdc =
        (const USBD_CDC_HandleTypeDef *)hUsbDeviceFS.pClassData;
    return (hcdc != NULL) && (hcdc->TxState == 0u);
}

/**
 * Copy one chunk into the staging buffer and send it, retrying while the
 * endpoint is busy.
 *
 * The wait for TxState==0 happens BEFORE the copy, not after the send.
 * Overwriting the staging buffer while the previous packet is still being
 * clocked out of it would corrupt that packet -- which is exactly the bug
 * this whole staging arrangement exists to avoid.
 */
static bool tx_chunk(const uint8_t *data, uint16_t len)
{
    /* ~1 s of patience at 2 ms a try. If the host is not draining the
     * port by then it is not listening at all, and blocking the whole
     * test tool forever would be worse than losing the line. */
    for (int retry = 0; retry < 500; retry++) {
        if (!usb_ready()) {
            return false;                  /* cable pulled mid-write */
        }
        if (tx_idle()) {
            memcpy(s_tx_stage, data, len);
            if (CDC_Transmit_FS(s_tx_stage, len) == USBD_OK) {
                return true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return false;
}

static bool tx_raw(const uint8_t *data, uint32_t len)
{
    if (data == NULL || len == 0u || !usb_ready()) {
        return false;
    }

    bool locked = false;
    if (s_tx_mutex != NULL) {
        locked = (xSemaphoreTake(s_tx_mutex, pdMS_TO_TICKS(1000)) == pdTRUE);
        if (!locked) {
            /* Someone else has held the port for a full second. Returning
             * rather than pushing on unlocked: two tasks interleaving into
             * one staging buffer is exactly the corruption being avoided. */
            return false;
        }
    }

    bool ok = true;
    uint32_t sent = 0u;
    while (sent < len) {
        uint32_t chunk = len - sent;
        if (chunk > TX_CHUNK) {
            chunk = TX_CHUNK;
        }
        if (!tx_chunk(&data[sent], (uint16_t)chunk)) {
            ok = false;
            break;
        }
        sent += chunk;
    }

    if (locked) {
        (void)xSemaphoreGive(s_tx_mutex);
    }
    return ok;
}

bool console_print(const char *s)
{
    if (s == NULL) {
        return false;
    }
    return tx_raw((const uint8_t *)s, (uint32_t)strlen(s));
}

bool console_println(const char *s)
{
    bool ok = console_print(s);
    /* CRLF, not bare LF: PuTTY and the Windows terminal both want the CR. */
    return console_print("\r\n") && ok;
}

bool console_printf(const char *fmt, ...)
{
    char    buf[224];
    va_list ap;

    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (n <= 0) {
        return false;
    }
    /* vsnprintf returns what it *would* have written, so clamp. */
    if ((size_t)n >= sizeof(buf)) {
        n = (int)sizeof(buf) - 1;
    }
    return tx_raw((const uint8_t *)buf, (uint32_t)n);
}

/* ================================================================== */
/*  Line editing                                                      */
/* ================================================================== */

/* Echo is batched rather than sent a byte at a time. One USB packet per
 * keystroke is fine for typing, but a paste arrives as a burst: 60
 * characters used to mean 60 separate transmits, each able to sit in
 * tx_chunk for up to a second waiting on the host, with the timeout check
 * out of reach the whole time. Collecting the echo and sending it in one
 * go turns that back into a single transfer. */
#define ECHO_MAX   64u

static void echo_flush(char *e, uint32_t *n)
{
    if (*n > 0u) {
        (void)tx_raw((const uint8_t *)e, *n);
        *n = 0u;
    }
}

/** Append at most 3 bytes, flushing first if they would not fit. */
static void echo_put(char *e, uint32_t *n, const char *s, uint32_t slen)
{
    if ((*n + slen) > ECHO_MAX) {
        echo_flush(e, n);
    }
    memcpy(&e[*n], s, slen);
    *n += slen;
}

/* Terminals variously send CR, LF or CRLF for Enter. s_last_eol (declared
 * with the other file statics above) remembers which arrived, so the other
 * half of a CRLF pair is discarded here rather than reaching the caller. */

bool console_readline(char *buf, uint32_t buf_len, uint32_t timeout_ms)
{
    if (buf == NULL || buf_len == 0u) {
        return false;
    }
    if (buf_len > CONSOLE_LINE_MAX) {
        buf_len = CONSOLE_LINE_MAX;
    }

    uint32_t         idx   = 0u;
    const TickType_t start = xTaskGetTickCount();
    const TickType_t limit = pdMS_TO_TICKS(timeout_ms);

    for (;;) {
        char     echo[ECHO_MAX];
        uint32_t elen      = 0u;
        bool     done      = false;
        bool     cancelled = false;
        uint8_t  c;

        while (console_getchar(&c)) {
            /* Swallow the second half of a CRLF from the previous line. */
            if (s_last_eol != 0u) {
                uint8_t prev = s_last_eol;
                s_last_eol = 0u;
                if ((prev == '\r' && c == '\n') || (prev == '\n' && c == '\r')) {
                    continue;
                }
            }

            if (c == '\r' || c == '\n') {
                s_last_eol = c;
                done = true;
                break;
            }
            if (c == 0x03u) {                        /* Ctrl-C: abandon */
                cancelled = true;
                break;
            }
            if (c == 0x08u || c == 0x7Fu) {          /* BS or DEL */
                if (idx > 0u) {
                    idx--;
                    /* Erase visually: back up, overwrite, back up again. */
                    echo_put(echo, &elen, "\b \b", 3u);
                }
                continue;
            }
            if (c >= 0x20u && c < 0x7Fu && idx < (buf_len - 1u)) {
                buf[idx++] = (char)c;
                /* Echo, since CDC has no local echo of its own. */
                echo_put(echo, &elen, (const char *)&c, 1u);
            }
            /* Anything else -- arrow keys, other control bytes, or a
             * character arriving after the line is full -- is dropped
             * silently and deliberately not echoed. */
        }

        echo_flush(echo, &elen);

        if (cancelled) {
            buf[0] = '\0';
            (void)console_print("^C\r\n");
            return true;
        }
        if (done) {
            buf[idx] = '\0';
            (void)console_print("\r\n");
            return true;
        }

        if (timeout_ms != 0u) {
            if ((xTaskGetTickCount() - start) > limit) {
                buf[idx] = '\0';
                return false;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));               /* poll, stay friendly */
    }
}
