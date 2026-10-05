/**
 * @file    test_protocol.c
 * @brief   Menu entry [8] that runs the protocol self-test.
 */

#include "bringup_config.h"

#if BRINGUP_TEST_PROTOCOL

#include "tests.h"
#include "console.h"
#include "protocol_selftest.h"

test_result_t test_protocol_run(void)
{
    (void)console_println("-- Protocol self-test (Phase 2A) ----------------");
    (void)console_println("    Pure software test: no hardware needed.");
    (void)console_println("    Verifies CRC, packet framing, encode/decode");
    (void)console_println("    loopback, corruption detection, and resync.");

    bool ok = protocol_selftest_run();

    if (ok) {
        (void)console_println("  Protocol layer is ready for Phase 2B (UART driver).");
        return TEST_PASS;
    } else {
        (void)console_println("  Fix the failures above before proceeding to Phase 2B.");
        return TEST_FAIL;
    }
}

#else  /* BRINGUP_TEST_PROTOCOL */

#include "tests.h"
test_result_t test_protocol_run(void) { return TEST_SKIP; }

#endif /* BRINGUP_TEST_PROTOCOL */
