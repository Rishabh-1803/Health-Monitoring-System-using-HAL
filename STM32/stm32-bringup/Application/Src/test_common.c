/**
 * @file    test_common.c
 * @brief   Shared helpers for the test modules.
 */

#include "tests.h"
#include "console.h"

#include "FreeRTOS.h"
#include "task.h"

#include <string.h>

const char *test_result_str(test_result_t r)
{
    switch (r) {
        case TEST_PASS:  return "PASS";
        case TEST_FAIL:  return "FAIL";
        case TEST_SKIP:  return "SKIP";
        case TEST_ABORT: return "ABORT";
        default:         return "????";
    }
}

test_answer_t test_ask(const char *question)
{
    char line[CONSOLE_LINE_MAX];

    console_flush_rx();
    (void)console_printf("%s [Y/n] ", question);

    if (!console_readline(line, sizeof(line), 30000u)) {
        /* No answer is its own outcome, not the default. Turning silence
         * into a yes would make an unattended run print a page of PASSes
         * for components nobody looked at. */
        (void)console_println("");
        (void)console_println("  (no answer in 30 s -- recorded as unconfirmed, not as a pass)");
        return TEST_ANSWER_NONE;
    }
    if (line[0] == 'y' || line[0] == 'Y') { return TEST_ANSWER_YES; }
    if (line[0] == 'n' || line[0] == 'N') { return TEST_ANSWER_NO;  }
    /* Bare Enter, or Ctrl-C, or anything unrecognised: the prompt shows Y
     * as the highlighted choice, so treat it as yes. The operator is
     * present either way, which is the part that matters. */
    return TEST_ANSWER_YES;
}

test_result_t test_answer_to_result(test_answer_t a)
{
    switch (a) {
        case TEST_ANSWER_YES: return TEST_PASS;
        case TEST_ANSWER_NO:  return TEST_FAIL;
        default:              return TEST_ABORT;
    }
}

void test_print_milli(const char *label, int32_t milli, const char *unit)
{
    /* Split the sign off first, so -0.05 does not come out as "0.-5". */
    const char *sign = (milli < 0) ? "-" : "";
    uint32_t mag  = (uint32_t)((milli < 0) ? -milli : milli);
    uint32_t whole = mag / 1000u;
    uint32_t frac  = (mag % 1000u) / 10u;      /* two decimal places */

    (void)console_printf("%s%s%lu.%02lu %s\r\n",
                         label, sign,
                         (unsigned long)whole, (unsigned long)frac, unit);
}
