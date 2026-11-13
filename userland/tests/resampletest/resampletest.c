// resampletest — os64/resample.h in ring 3: libsound.so loaded, its two
// libraries behind it, and a sine through it coming out the same sine
// (checks.h is the body, and says what is asserted).
//
// Exit codes: 0x5A3E0000 success, 0x5A3E00NN the check that failed.

#include "os64/os64.h"
#include "checks.h"

static void say(const char *what, long value)
{
    os64_printf("resampletest: %s: %ld\n", what, value);
}

int main(void)
{
    int failed = resample_checks(say);
    if (failed != 0)
    {
        os64_printf("resampletest: FAIL check %d\n", failed);
        return (int)(0x5A3E0000 | failed);
    }
    os64_printf("resampletest: all checks passed\n");
    return (int)0x5A3E0000;
}
