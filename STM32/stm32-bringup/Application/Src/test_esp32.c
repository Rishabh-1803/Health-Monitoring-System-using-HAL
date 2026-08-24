/**
 * @file    test_esp32.c
 * @brief   USART2 link on PA2/PA3 -- the wire to the ESP32.
 *
 * Two modes, because they answer different questions.
 *
 *   Loopback  jumper PA2 straight to PA3 and the STM32 talks to itself.
 *             This isolates the STM32 half completely: if loopback fails
 *             the problem is the pins, the clock or the UART config, and
 *             there is no point looking at the ESP32 at all.
 *
 *   Echo      send a line to the ESP32 and print whatever comes back.
 *             This is the real end-to-end check, and it only means
 *             anything once loopback passes.
 *
 * Deliberately at 115200 rather than the 460800 the application will
 * eventually use. Prove the wiring at a forgiving baud rate first;
 * marginal wiring passes at 115200 and fails at 460800, and you want
 * those two failures to be distinguishable.
 *
 * Echo mode judges the bytes, not the byte count. A baud-rate mismatch
 * delivers a healthy-looking stream of nonsense, so "something arrived"
 * is not allowed to become a PASS -- see the end of echo_mode().
 *
 * Reception is polled a byte at a time rather than using interrupts.
 * CubeMX enables the USART2 IRQ but nothing arms an interrupt-mode
 * receive, so the RXNE interrupt is never enabled and blocking receive
 * is safe. Sending one byte then immediately reading one byte also means
 * the single-byte receive register can never overrun.
 */

#include "bringup_config.h"

#if BRINGUP_TEST_ESP32_LINK

#include "tests.h"
#include "console.h"
#include "bsp.h"
#include "main.h"
#include "usart.h"

#include "FreeRTOS.h"
#include "task.h"

#include <string.h>

/* A pattern with alternating bits and both extremes -- 0x00 and 0xFF
 * catch framing and idle-level faults that ASCII would slide past. */
static const uint8_t PATTERN[] = {
    0x55u, 0xAAu, 0x00u, 0xFFu, 0x01u, 0x80u, 0x0Fu, 0xF0u,
    'S',   'T',   'M',   '3',   '2',   '-',   'O',   'K'
};

/**
 * Clear the error flags and drop whatever byte is sitting in DR.
 *
 * Exactly ONE macro call, deliberately. On the F4 the clear sequence for
 * PE, FE, NE and ORE is "read SR, then read DR", and the HAL header makes
 * __HAL_UART_CLEAR_OREFLAG, _NEFLAG and _FEFLAG plain aliases of
 * __HAL_UART_CLEAR_PEFLAG -- verified in stm32f4xx_hal_uart.h, lines
 * 502-532. Calling all four therefore reads DR four times, and a fifth
 * explicit read to "drain" it makes five. Each of those reads pops the
 * receive register, so a byte that arrives mid-sequence is silently eaten.
 * One call clears every flag and drains DR at the same time.
 *
 * The reason the drain matters is not the error flags themselves. A stale
 * byte left in DR by a previous run is returned as the answer to the byte
 * this run has just sent, which shifts the whole send-one/receive-one
 * comparison by one position and turns a working link into 16 mismatches.
 */
static void uart_clear_errors(void)
{
    __HAL_UART_CLEAR_PEFLAG(&huart2);
}

static test_result_t loopback_mode(void)
{
    (void)console_println("  Loopback: jumper PA2 (TX) directly to PA3 (RX).");

    /* Only an explicit yes may proceed. Without the jumper this test
     * cannot pass, so running it anyway just prints 16 failures and
     * invites the operator to go looking for a UART fault that is not
     * there. Silence is treated the same as no. */
    test_answer_t jumper = test_ask("  Is the jumper fitted?");
    if (jumper != TEST_ANSWER_YES) {
        (void)console_println("  Skipped: fit the jumper and run this again. Loopback is the");
        (void)console_println("  test that tells you whether the STM32 half works at all.");
        return TEST_SKIP;
    }

    uart_clear_errors();

    int good = 0;
    int bad  = 0;

    for (unsigned i = 0; i < sizeof(PATTERN); i++) {
        uint8_t tx = PATTERN[i];
        uint8_t rx = 0u;

        if (HAL_UART_Transmit(&huart2, &tx, 1u, 50u) != HAL_OK) {
            (void)console_println("  Transmit itself timed out -- the UART is not running.");
            return TEST_FAIL;
        }
        HAL_StatusTypeDef st = HAL_UART_Receive(&huart2, &rx, 1u, 50u);

        if (st != HAL_OK) {
            bad++;
            (void)console_printf("    sent 0x%02X  ->  nothing came back\r\n", tx);
            uart_clear_errors();
        } else if (rx != tx) {
            bad++;
            (void)console_printf("    sent 0x%02X  ->  got 0x%02X   MISMATCH\r\n", tx, rx);
        } else {
            good++;
        }
    }

    (void)console_printf("  %d of %u bytes returned correctly.\r\n",
                         good, (unsigned)sizeof(PATTERN));

    if (good == (int)sizeof(PATTERN)) {
        (void)console_println("  The STM32 side of the link is proven good at 115200 8N1.");
        return TEST_PASS;
    }
    if (good == 0) {
        (void)console_println("  Nothing returned at all. Either the jumper is not actually");
        (void)console_println("  connecting PA2 to PA3, or USART2 is not clocked.");
        return TEST_FAIL;
    }
    (void)console_println("  Partial success means marginal timing -- usually a bad");
    (void)console_println("  jumper contact rather than a clock problem.");
    return TEST_FAIL;
}

/** Printable ASCII, or the whitespace that legitimately frames a line. */
static bool is_sane_char(uint8_t b)
{
    return (b >= 0x20u && b < 0x7Fu) || (b == '\r') || (b == '\n') || (b == '\t');
}

/**
 * Print the received bytes as text and as hex.
 *
 * Both dumps are built into one buffer and sent as whole lines. The
 * obvious version -- console_print() per byte -- costs one USB CDC packet
 * and one mutex round trip per character, so a 96-byte reply becomes 96
 * transactions and takes long enough to look like a hang.
 */
static void dump_bytes(const uint8_t *buf, uint32_t n)
{
    static const char HEX[] = "0123456789ABCDEF";
    char out[128];
    uint32_t k;

    /* ---- as text ---- */
    k = 0u;
    for (uint32_t i = 0u; i < n && k < (sizeof(out) - 1u); i++) {
        out[k++] = (buf[i] >= 0x20u && buf[i] < 0x7Fu) ? (char)buf[i] : '.';
    }
    out[k] = '\0';
    (void)console_printf("  As text: \"%s\"\r\n", out);

    /* ---- as hex, 16 bytes to a line ---- */
    for (uint32_t base = 0u; base < n; base += 16u) {
        uint32_t end = ((base + 16u) < n) ? (base + 16u) : n;
        k = 0u;
        for (uint32_t i = base; i < end; i++) {
            out[k++] = ' ';
            out[k++] = HEX[(buf[i] >> 4) & 0x0Fu];
            out[k++] = HEX[buf[i] & 0x0Fu];
        }
        out[k] = '\0';
        (void)console_printf("  %s %s\r\n",
                             (base == 0u) ? "As hex :" : "        ", out);
    }
}

static test_result_t echo_mode(void)
{
    (void)console_println("  Echo probe: cross the wires and share a ground.");
    (void)console_println("    STM32 PA2 (TX) -> ESP32 RX");
    (void)console_println("    STM32 PA3 (RX) -> ESP32 TX");
    (void)console_println("    GND            -> GND");
    (void)console_println("  Both sides are 3V3, so no level shifter is needed.");

    uart_clear_errors();

    static const char probe[] = "STM32-BRINGUP-PING\r\n";
    (void)console_printf("  Sending: %s", probe);

    if (HAL_UART_Transmit(&huart2, (uint8_t *)probe,
                          (uint16_t)strlen(probe), 200u) != HAL_OK) {
        (void)console_println("  Transmit timed out -- USART2 is not working.");
        return TEST_FAIL;
    }

    (void)console_printf("  Listening for %lu ms ...\r\n",
                         (unsigned long)ESP32_LINK_PROBE_TIMEOUT_MS);

    uint8_t  buf[96];
    uint32_t n = 0u;
    const TickType_t start = xTaskGetTickCount();

    while (((xTaskGetTickCount() - start) < pdMS_TO_TICKS(ESP32_LINK_PROBE_TIMEOUT_MS))
           && (n < sizeof(buf))) {
        uint8_t b;
        if (HAL_UART_Receive(&huart2, &b, 1u, 20u) == HAL_OK) {
            buf[n++] = b;
        } else {
            uart_clear_errors();
        }
    }

    if (n == 0u) {
        (void)console_println("  Nothing received.");
        (void)console_println("  Run loopback first. If loopback passes, the STM32 is fine");
        (void)console_println("  and the problem is on the ESP32 side: wrong baud rate, TX");
        (void)console_println("  and RX not crossed, no common ground, or no firmware there");
        (void)console_println("  that replies. An ESP32 that only listens will never answer.");
        return TEST_FAIL;
    }

    (void)console_printf("  Received %lu byte(s).\r\n", (unsigned long)n);
    dump_bytes(buf, n);

    /* Bytes arriving is not the same as a working link, so the verdict is
     * decided on the content rather than on the count. A baud-rate
     * mismatch delivers a steady stream of bytes that happen to be
     * nonsense, and reporting that as a PASS is exactly the failure this
     * tool exists to catch. */
    uint32_t sane = 0u;
    for (uint32_t i = 0u; i < n; i++) {
        if (is_sane_char(buf[i])) { sane++; }
    }

    if (sane == 0u) {
        (void)console_println("  Not one byte is printable. Something is transmitting, so the");
        (void)console_println("  wire and the grounds are good -- but the framing is wrong.");
        (void)console_println("  Almost always the ESP32 is at a different baud rate; 74880");
        (void)console_println("  (its boot log) and 9600 are the usual culprits.");
        return TEST_FAIL;
    }

    if (sane < n) {
        (void)console_printf("  %lu of %lu bytes are not printable.\r\n",
                             (unsigned long)(n - sane), (unsigned long)n);
        (void)console_println("  A partly-corrupt reply means marginal timing rather than a");
        (void)console_println("  wrong baud rate: long jumpers, a shared ground carrying the");
        (void)console_println("  relay current, or the ESP32 clock drifting. Not a pass.");
        return TEST_FAIL;
    }

    (void)console_println("  Every byte is printable, so the baud rate and framing agree.");
    (void)console_println("  What the ESP32 actually said is not checked -- read the text");
    (void)console_println("  above and judge that yourself.");
    return TEST_PASS;
}

test_result_t test_esp32_link_run(void)
{
    char line[CONSOLE_LINE_MAX];

    (void)console_println("-- ESP32 link, USART2 on PA2/PA3 at 115200 -------");
    (void)console_println("    [l] loopback  (PA2 jumpered to PA3, STM32 only)");
    (void)console_println("    [e] echo      (talk to the ESP32)");
    (void)console_println("    [q] back");

    console_flush_rx();
    (void)console_print("  Choose: ");
    if (!console_readline(line, sizeof(line), 30000u)) {
        return TEST_ABORT;
    }

    switch (line[0]) {
        case 'l': case 'L': return loopback_mode();
        case 'e': case 'E': return echo_mode();
        default:            return TEST_SKIP;
    }
}

#else  /* BRINGUP_TEST_ESP32_LINK */

#include "tests.h"
test_result_t test_esp32_link_run(void) { return TEST_SKIP; }

#endif /* BRINGUP_TEST_ESP32_LINK */
