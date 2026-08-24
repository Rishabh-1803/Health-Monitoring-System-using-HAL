/**
 * @file    test_vibration.c
 * @brief   SW-420 style vibration switch on PB12.
 *
 * A vibration switch is a mechanical contact, so the only meaningful
 * test is to watch for edges while the operator taps the board. The
 * test counts transitions with a short debounce and reports the idle
 * level first -- knowing whether the switch rests open or closed is
 * what tells you if VIBRATION_ACTIVE_LOW is set correctly.
 *
 * A count of zero is reported as inconclusive rather than a failure:
 * an untapped sensor and a broken one look identical from here, and
 * claiming otherwise would be guessing.
 */

#include "bringup_config.h"

#if BRINGUP_TEST_VIBRATION

#include "tests.h"
#include "console.h"
#include "bsp.h"

#include "FreeRTOS.h"
#include "task.h"

test_result_t test_vibration_run(void)
{
    (void)console_println("-- Vibration switch on PB12 ----------------------");

    /* Idle level, sampled over 100 ms so a single bounce cannot mislead. */
    int asserted_count = 0;
    for (int i = 0; i < 10; i++) {
        if (bsp_vibration_asserted()) { asserted_count++; }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    bool idle_asserted = (asserted_count > 5);
    (void)console_printf("  Idle state: %s (%d of 10 samples asserted)\r\n",
                         idle_asserted ? "ASSERTED" : "not asserted",
                         asserted_count);

    if (idle_asserted) {
        (void)console_println("  Asserted while still is usually the wrong polarity.");
        (void)console_println("  If tapping makes it read 'not asserted', flip");
        (void)console_println("  VIBRATION_ACTIVE_LOW in bringup_config.h.");
    }

    (void)console_println("  Tap or shake the sensor now -- watching for 15 s.");
    (void)console_println("  Press any key to stop early.");
    console_flush_rx();

    bool     last     = bsp_vibration_asserted();
    uint32_t edges    = 0u;
    uint32_t last_edge_tick = 0u;
    const TickType_t start = xTaskGetTickCount();

    while ((xTaskGetTickCount() - start) < pdMS_TO_TICKS(15000)) {
        bool now = bsp_vibration_asserted();

        if (now != last) {
            /* 20 ms debounce: a mechanical contact rings for a few ms and
             * would otherwise inflate the count into the hundreds. */
            uint32_t tick = (uint32_t)xTaskGetTickCount();
            if ((tick - last_edge_tick) > pdMS_TO_TICKS(20)) {
                edges++;
                last_edge_tick = tick;
                (void)console_printf("\r    edges: %lu   (now %s)      ",
                                    (unsigned long)edges,
                                    now ? "ASSERTED    " : "not asserted");
            }
            last = now;
        }

        if (console_key_pressed()) {
            console_flush_rx();
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    (void)console_println("");
    (void)console_printf("  Counted %lu debounced transitions.\r\n",
                         (unsigned long)edges);

    if (edges == 0u) {
        (void)console_println("  No transitions. Either the sensor was not disturbed, or");
        (void)console_println("  it is not wired to PB12. Check DO -> PB12, VCC to 3V3,");
        (void)console_println("  GND common; on modules with a pot, adjust the threshold.");
        (void)console_println("  Reported as inconclusive, not failed -- firmware cannot");
        (void)console_println("  tell an untapped sensor from a dead one.");
        return TEST_ABORT;
    }

    if (edges < 3u) {
        (void)console_println("  Very few edges: it responds, but the threshold may be");
        (void)console_println("  set too high. Turn the pot down if it has one.");
    }

    return TEST_PASS;
}

#else  /* BRINGUP_TEST_VIBRATION */

#include "tests.h"
test_result_t test_vibration_run(void) { return TEST_SKIP; }

#endif /* BRINGUP_TEST_VIBRATION */
