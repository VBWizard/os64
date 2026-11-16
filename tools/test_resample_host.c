// test_resample_host.c — userland/tests/resampletest/checks.h on the host,
// against the same two translation units libsound.so is built from, under
// ASan and UBSan. See tools/test_resample_host.sh.

#include <stdio.h>

#include "checks.h"

static void say(const char *what, long value)
{
    printf("  %s: %ld\n", what, value);
}

int main(void)
{
    int failed = resample_checks(say);
    if (failed != 0)
    {
        printf("test_resample_host: FAIL check %d\n", failed);
        return 1;
    }
    printf("test_resample_host: all checks passed\n");
    return 0;
}
