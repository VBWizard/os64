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
} test_case_t;

bool test_register(const char *name, bool (*func)(void));
void test_framework_init(void);
void test_run_all(void);

#endif
