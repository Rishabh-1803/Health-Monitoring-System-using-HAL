/**
 * @file    test_gpio.c
 * @brief   Onboard LED, buzzer and relay -- the pure output test.
 *
 * These three have nothing to read back, so the firmware drives a
 * pattern the operator can unambiguously recognise and then asks what
 * happened. A blink you cannot count is not evidence; five slow blinks
 * are.
 *
 * The onboard LED needs one piece of coordination: CubeMX's LEDTask
 * owns PC13 and toggles it every 2 s as a liveness heartbeat. Left
 * running it would fight this test, so the task is suspended for the
 * duration and resumed afterwards.
 */

#include "bringup_config.h"

#if BRINGUP_TEST_GPIO

#include "tests.h"
#include "console.h"
#include "bsp.h"

#include "FreeRTOS.h"
#include "task.h"
#include "cmsis_os.h"

extern osThreadId_t LEDTaskHandle;    /* defined in Core/Src/freertos.c */

/* ------------------------------------------------------------------ */

static const char *verdict_word(test_answer_t a)
{
    switch (a) {
        case TEST_ANSWER_YES: return "ok";
        case TEST_ANSWER_NO:  return "NO";
        default:              return "unconfirmed";
    }
}

static test_answer_t led_subtest(void)
{
    (void)console_println("  LED  : 5 slow blinks on PC13 (the onboard LED).");
    (void)console_println("         Note: PC13 is active LOW on the Blackpill.");

    /* Take PC13 away from the heartbeat task for the duration. */
    bool suspended = false;
    if (LEDTaskHandle != NULL) {
        if (osThreadSuspend(LEDTaskHandle) == osOK) {
            suspended = true;
        }
    }

    for (int i = 0; i < 5; i++) {
        bsp_led_set(true);
        vTaskDelay(pdMS_TO_TICKS(300));
        bsp_led_set(false);
        vTaskDelay(pdMS_TO_TICKS(300));
    }

    test_answer_t a = test_ask("  LED  : did it blink 5 times?");

    if (suspended) {
        (void)osThreadResume(LEDTaskHandle);
    }
    return a;
}

static test_answer_t buzzer_subtest(void)
{
#if BUZZER_IS_ACTIVE_TYPE
    (void)console_println("  BUZZ : active buzzer -- 3 short beeps.");
    for (int i = 0; i < 3; i++) {
        /* An active buzzer only wants a level, so any argument turns it
         * on -- see the note on bsp_buzzer_tone in bsp.h. It is silenced
         * with bsp_buzzer_off(), never with tone(0). */
        bsp_buzzer_tone(0u);
        vTaskDelay(pdMS_TO_TICKS(150));
        bsp_buzzer_off();
        vTaskDelay(pdMS_TO_TICKS(150));
    }
#else
    (void)console_println("  BUZZ : passive buzzer -- sweeping 1000, 2000, 3000 Hz.");
    static const uint32_t tones[] = { 1000u, 2000u, 3000u };
    for (unsigned i = 0; i < (sizeof(tones) / sizeof(tones[0])); i++) {
        (void)console_printf("         %lu Hz\r\n", (unsigned long)tones[i]);
        bsp_buzzer_tone(tones[i]);
        vTaskDelay(pdMS_TO_TICKS(400));
    }
    bsp_buzzer_off();
#endif

    test_answer_t a = test_ask("  BUZZ : did you hear it?");
    if (a == TEST_ANSWER_NO) {
        (void)console_println("         If silent: check the buzzer type. A passive");
        (void)console_println("         buzzer needs the PWM (this build); an active one");
        (void)console_println("         needs BUZZER_IS_ACTIVE_TYPE 1 in bringup_config.h.");
    }
    return a;
}

static test_answer_t relay_subtest(void)
{
    (void)console_println("  RELAY: 3 on/off cycles -- listen for the click.");
    for (int i = 0; i < 3; i++) {
        bsp_relay_set(true);
        vTaskDelay(pdMS_TO_TICKS(500));
        bsp_relay_set(false);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    test_answer_t a = test_ask("  RELAY: did it click 3 times?");
    if (a == TEST_ANSWER_NO) {
        (void)console_println("         If dead: relay boards need their own supply on");
        (void)console_println("         JD-VCC (5 V), not the 3V3 rail. Also try flipping");
        (void)console_println("         RELAY_ACTIVE_LOW in bringup_config.h.");
    }
    return a;
}

test_result_t test_gpio_run(void)
{
    (void)console_println("-- Outputs: LED / buzzer / relay ------------------");

    test_answer_t led   = led_subtest();
    test_answer_t buzz  = buzzer_subtest();
    test_answer_t relay = relay_subtest();

    (void)console_printf("  Result: LED %s, buzzer %s, relay %s\r\n",
                         verdict_word(led),
                         verdict_word(buzz),
                         verdict_word(relay));

    /* An explicit NO outranks silence: something is definitely broken, and
     * that is the more useful headline. Silence alone is only ever ABORT,
     * never PASS -- nobody was there to see the LED. */
    if (led == TEST_ANSWER_NO || buzz == TEST_ANSWER_NO || relay == TEST_ANSWER_NO) {
        return TEST_FAIL;
    }
    if (led == TEST_ANSWER_NONE || buzz == TEST_ANSWER_NONE ||
        relay == TEST_ANSWER_NONE) {
        (void)console_println("  At least one output went unconfirmed. These three cannot");
        (void)console_println("  be measured by the firmware, so nothing is proven here");
        (void)console_println("  without someone watching. Rerun with option 1.");
        return TEST_ABORT;
    }
    return TEST_PASS;
}

#else  /* BRINGUP_TEST_GPIO */

#include "tests.h"
test_result_t test_gpio_run(void) { return TEST_SKIP; }

#endif /* BRINGUP_TEST_GPIO */
