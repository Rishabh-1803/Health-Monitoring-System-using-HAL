/**
 * @file    test_adc.c
 * @brief   Analog input on PA1 (ADC1_IN1), aimed at a current sensor.
 *
 * The test does three things a single reading cannot: it takes an
 * averaged reading to establish the level, it measures peak-to-peak
 * noise over 200 samples, and it watches the value while the operator
 * changes the input. A stuck ADC returns a beautifully stable number,
 * so stability alone proves nothing -- movement is the real evidence.
 *
 * ACS712 numbers are printed as an estimate only. The scaling constants
 * come from the datasheet, not from a calibration of this board, so the
 * current figure is indicative. The millivolt reading is the measurement.
 */

#include "bringup_config.h"

#if BRINGUP_TEST_ADC

#include "tests.h"
#include "console.h"
#include "bsp.h"

#include "FreeRTOS.h"
#include "task.h"

test_result_t test_adc_run(void)
{
    (void)console_println("-- ADC1_IN1 on PA1 -------------------------------");
    (void)console_println("  WARNING: 3V3 maximum on this pin. An ACS712 runs from 5 V");
    (void)console_println("  and idles near 2.5 V, rising under load -- it can exceed");
    (void)console_println("  3V3. Use a divider or a 3V3-native sensor.");

    /* ---- averaged level ---- */
    uint32_t to_before = bsp_adc_timeouts();
    uint16_t raw = bsp_adc_read_avg(64u);
    uint32_t mv  = bsp_adc_raw_to_mv(raw);

    /* A conversion that times out returns 0, which is indistinguishable
     * from a genuine 0 V by looking at the number alone. Ask the BSP
     * whether that happened rather than guessing from the value. */
    if (bsp_adc_timeouts() != to_before) {
        (void)console_printf("  %lu conversion(s) timed out waiting for EOC.\r\n",
                             (unsigned long)(bsp_adc_timeouts() - to_before));
        (void)console_println("  This is the ADC peripheral itself, not the sensor: the");
        (void)console_println("  clock, the prescaler or ADON. Nothing below is a reading,");
        (void)console_println("  because a timed-out conversion reports 0 and would");
        (void)console_println("  otherwise look exactly like a grounded pin.");
        return TEST_FAIL;
    }

    (void)console_printf("  Averaged over 64 samples: raw %u / 4095\r\n", raw);
    test_print_milli("  Voltage: ", (int32_t)mv, "V   (mV/1000)");

    /* ---- noise ---- */
    uint16_t lo = 0xFFFFu, hi = 0u;
    uint32_t sum = 0u;
    for (int i = 0; i < 200; i++) {
        uint16_t v = bsp_adc_read_raw();
        if (v < lo) { lo = v; }
        if (v > hi) { hi = v; }
        sum += v;
    }
    uint16_t mean = (uint16_t)(sum / 200u);
    uint16_t p2p  = (uint16_t)(hi - lo);

    (void)console_printf("  200 samples: min %u, max %u, mean %u, peak-to-peak %u\r\n",
                         lo, hi, mean, p2p);
    (void)console_printf("  Noise is about %lu mV peak-to-peak.\r\n",
                         (unsigned long)bsp_adc_raw_to_mv(p2p));

    if (p2p > 200u) {
        (void)console_println("  That is a lot of noise. Check the ground return, keep the");
        (void)console_println("  analog wire away from the buzzer and relay, and add a");
        (void)console_println("  100nF cap from the pin to ground if it persists.");
    }

    /* ---- an ACS712 interpretation, clearly labelled as an estimate ---- */
    int32_t delta_mv = (int32_t)mv - (int32_t)ACS712_ZERO_MV;
    int32_t milli_a  = (delta_mv * 1000) / (int32_t)ACS712_MV_PER_AMP;
    (void)console_printf("  If this is an ACS712 (%lu mV/A, zero at %lu mV):\r\n",
                         (unsigned long)ACS712_MV_PER_AMP,
                         (unsigned long)ACS712_ZERO_MV);
    test_print_milli("    estimated current: ", milli_a, "A (uncalibrated)");

    /* ---- the part that actually proves the ADC works ---- */
    (void)console_println("");
    (void)console_println("  Now change the input: turn a pot, or load the sensor.");
    (void)console_println("  Reading for 10 s, then reporting whether it moved.");
    (void)console_println("  Press any key to stop early.");

    uint16_t start_v = bsp_adc_read_avg(16u);
    uint16_t min_v = start_v, max_v = start_v;
    console_flush_rx();

    for (int i = 0; i < 20; i++) {
        vTaskDelay(pdMS_TO_TICKS(500));
        uint16_t v = bsp_adc_read_avg(16u);
        if (v < min_v) { min_v = v; }
        if (v > max_v) { max_v = v; }

        (void)console_printf("\r    raw %4u  =  %4lu mV   ",
                            v, (unsigned long)bsp_adc_raw_to_mv(v));

        if (console_key_pressed()) {
            console_flush_rx();
            break;
        }
    }
    (void)console_println("");

    uint16_t swing = (uint16_t)(max_v - min_v);
    (void)console_printf("  Total swing during the window: %u counts (%lu mV)\r\n",
                         swing, (unsigned long)bsp_adc_raw_to_mv(swing));

    /* Verdicts. A rail-pinned reading is the one unambiguous failure. */
    if (raw == 0u && swing == 0u) {
        (void)console_println("  Dead at zero and never moved: pin grounded, floating, or");
        (void)console_println("  the conversion is not completing.");
        return TEST_FAIL;
    }
    if (raw >= 4090u && swing == 0u) {
        (void)console_println("  Pinned at full scale: the input is at or above 3V3.");
        (void)console_println("  Disconnect it before something is damaged.");
        return TEST_FAIL;
    }
    if (swing < 8u) {
        (void)console_println("  The value never moved. The ADC is converting, but nothing");
        (void)console_println("  proves the sensor responds -- retry while changing the input.");
        return TEST_ABORT;
    }

    (void)console_println("  The reading tracked the input: ADC and sensor both alive.");
    return TEST_PASS;
}

#else  /* BRINGUP_TEST_ADC */

#include "tests.h"
test_result_t test_adc_run(void) { return TEST_SKIP; }

#endif /* BRINGUP_TEST_ADC */
