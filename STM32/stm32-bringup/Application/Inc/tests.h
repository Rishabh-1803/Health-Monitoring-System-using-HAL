/**
 * @file    tests.h
 * @brief   One entry point per component test.
 *
 * Every test follows the same contract: print what it is about to do,
 * do it, print the value it actually measured, and return a verdict.
 * No test may hang -- each one either times out or can be aborted with
 * a keypress, because a bring-up tool that locks up tells you nothing.
 *
 * Where a component has no readable output (an LED, a buzzer, a relay)
 * the test asks the operator to confirm what they saw or heard. That is
 * deliberately not reported as an automatic PASS: the firmware genuinely
 * cannot know whether the LED lit.
 *
 * The same principle governs what happens when nobody answers. An
 * unanswered question is never resolved into a PASS -- it becomes
 * TEST_ABORT, "nobody was watching". A tool that quietly awards itself a
 * pass for a question the operator never saw is worse than no tool: it
 * produces a clean-looking report about hardware that was never checked.
 */

#ifndef TESTS_H
#define TESTS_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    TEST_PASS = 0,   /* measured, and the measurement was sane        */
    TEST_FAIL,       /* measured, and it was wrong or absent          */
    TEST_SKIP,       /* compiled out, or the operator declined        */
    TEST_ABORT       /* operator pressed a key, or a timeout expired  */
} test_result_t;

/** Reply to a console question. Deliberately three-valued. */
typedef enum {
    TEST_ANSWER_YES = 0,
    TEST_ANSWER_NO,
    TEST_ANSWER_NONE   /* timed out -- nobody was at the terminal */
} test_answer_t;

/** "PASS" / "FAIL" / "SKIP" / "ABORT" for printing. */
const char *test_result_str(test_result_t r);

/**
 * Ask a yes/no question on the console, waiting 30 s.
 *
 * Returns TEST_ANSWER_NONE if the time expires. Callers must not fold
 * that into yes: it means the question went unread, which is a different
 * fact from "the operator says it did not work". Bare Enter counts as
 * yes, since the prompt shows Y as the highlighted choice.
 */
test_answer_t test_ask(const char *question);

/**
 * Map an answer to a verdict for the common case where yes means the
 * component worked: yes -> PASS, no -> FAIL, no answer -> ABORT.
 */
test_result_t test_answer_to_result(test_answer_t a);

/**
 * Print a signed value that is scaled by 1000, as a decimal with two
 * places -- e.g. -1234 prints as "-1.23". Used instead of %f so the
 * build does not need floating-point printf.
 */
void test_print_milli(const char *label, int32_t milli, const char *unit);

/* ------------------------------------------------------------------ */
/*  The tests                                                         */
/* ------------------------------------------------------------------ */

test_result_t test_gpio_run(void);        /* onboard LED, buzzer, relay  */
test_result_t test_i2c_scan_run(void);    /* who is on the bus           */
test_result_t test_oled_run(void);        /* SSD1306 panel               */
test_result_t test_ds18b20_run(void);     /* 1-Wire temperature          */
test_result_t test_adc_run(void);         /* analog / current sensor     */
test_result_t test_vibration_run(void);   /* digital vibration switch    */
test_result_t test_esp32_link_run(void);  /* USART2 to the ESP32         */

#endif /* TESTS_H */
