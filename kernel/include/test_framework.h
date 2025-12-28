#ifndef TEST_FRAMEWORK_H
#define TEST_FRAMEWORK_H
#include "panic.h"

#include <stdbool.h>
#include <stddef.h>

#include "printd.h"

#define TEST_MAX_CASES 64

#define TEST_FAIL(msg) panic("    FAIL: %s\n", msg); 

typedef struct test_case {
    const char *name;
    bool (*func)(void);
    int phase;
} test_case_t;

enum {
    TEST_PHASE_PREBOOT = 0,
    TEST_PHASE_POSTBOOT = 1
};

bool test_register(const char *name, bool (*func)(void), int phase);
void test_framework_init(void);
void test_run_preboot(void);
void test_run_postboot(void);

#endif
