#ifndef TEST_FRAMEWORK_H
#define TEST_FRAMEWORK_H
#include "panic.h"

#include <stdbool.h>
#include <stddef.h>

#include "printd.h"

#define TEST_MAX_CASES 64

#define TEST_FAIL(msg) panic("    FAIL: %s\n", msg); 

// What a FAILURE of this test means for the boot (2026-08-08 — Chris's
// ruling: "a failed test means I want to do analysis, not stare at a
// panic"). The trio is ext2's own s_errors enum reborn — continue /
// remount-ro / panic — because Rémy Card had this exact argument with
// himself in 1993 and put all three answers in the superblock:
//   TEST_WARN  — report on the glass and the log, keep booting. The
//                default for postboot: a content check's opinion should
//                cost analysis time, not uptime.
//   TEST_RO    — the failure impeaches the WRITE path: every mount is
//                demoted to read-only (vfs_demote_all_mounts_readonly) so
//                nothing compounds the damage — logd included — but the
//                system stays up for analysis.
//   TEST_PANIC — the failure impeaches the substrate itself; running on
//                it proves nothing and risks everything. The default for
//                preboot (kernel invariants).
// The TESTS= cmdline knob overrides every test's policy for a boot:
// TESTS=panic restores the old halt-on-any-failure strictness (CI-flavored
// entries), TESTS=warn forces continue-always (bare-metal triage).
typedef enum {
    TEST_POLICY_WARN  = 0,
    TEST_POLICY_RO    = 1,
    TEST_POLICY_PANIC = 2
} test_policy_t;

typedef struct test_case {
    const char *name;
    bool (*func)(void);
    int phase;
    test_policy_t policy;
} test_case_t;

enum {
    TEST_PHASE_PREBOOT = 0,
    TEST_PHASE_POSTBOOT = 1
};

// Registers with the phase's default policy (preboot=PANIC, postboot=WARN).
bool test_register(const char *name, bool (*func)(void), int phase);
// For the tests whose failure means more than "interesting" — the write
// gauntlets register with TEST_POLICY_RO here.
bool test_register_policy(const char *name, bool (*func)(void), int phase,
                          test_policy_t policy);
void test_framework_init(void);
void test_run_preboot(void);
void test_run_postboot(void);

#endif
