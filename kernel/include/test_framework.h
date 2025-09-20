#ifndef TEST_FRAMEWORK_H
#define TEST_FRAMEWORK_H

#include <stdbool.h>
#include <stddef.h>

#include "printd.h"

#define TEST_MAX_CASES 64

#define TEST_FAIL(msg) do { printd(DEBUG_TESTS, "    FAIL: %s\n", msg); return false; } while (0)

typedef struct test_case {
    const char *name;
    bool (*func)(void);
} test_case_t;

bool test_register(const char *name, bool (*func)(void));
void test_framework_init(void);
void test_run_all(void);

#endif
