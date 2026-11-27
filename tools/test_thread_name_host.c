// The thread name rule (abi/include/os64/thread_name.h): every length from
// empty to past the limit, every byte value at every place it matters.
//
//   test_thread_name

#include <stdio.h>
#include <string.h>

#include "os64/thread_name.h"

static unsigned long s_checks, s_failed;

static void check(int ok, const char *what, int a, int b)
{
    s_checks++;
    if (!ok) {
        s_failed++;
        if (s_failed <= 20)
            printf("FAIL: %s (%d, %d)\n", what, a, b);
    }
}

int main(void)
{
    char name[OS64_THREAD_NAME_MAX + 8];

    // Lengths: 0..63 bytes of a printable letter are taken; 64 and more are
    // not, the terminator falling outside the limit.
    for (int len = 0; len <= OS64_THREAD_NAME_MAX + 4; len++) {
        memset(name, 'a', sizeof(name));
        name[len] = '\0';
        check(os64_thread_name_valid(name) == (len < OS64_THREAD_NAME_MAX), "length", len, 0);
    }

    // Bytes: each value at the first, a middle and the last place of a
    // 63-byte name. Printable ASCII is taken, everything else refused (NUL
    // there ends the name early, which is a shorter, valid name).
    int places[] = { 0, 31, OS64_THREAD_NAME_MAX - 2 };
    for (int p = 0; p < 3; p++)
        for (int c = 1; c < 256; c++) {
            memset(name, 'b', sizeof(name));
            name[OS64_THREAD_NAME_MAX - 1] = '\0';
            name[places[p]] = (char)c;
            check(os64_thread_name_valid(name) == (c >= 0x20 && c <= 0x7E), "byte", c, places[p]);
        }

    // The edges named: space and tilde taken, unit separator and DEL not.
    check(os64_thread_name_valid(" ") && os64_thread_name_valid("~"), "space and tilde", 0x20, 0x7E);
    check(!os64_thread_name_valid("\x1f") && !os64_thread_name_valid("\x7f"), "0x1f and 0x7f", 0x1F, 0x7F);
    check(os64_thread_name_valid("h2-reader") && os64_thread_name_valid("worker-01"), "examples", 0, 0);

    printf("thread name rule: %lu checks, %lu failed\n", s_checks, s_failed);
    return s_failed != 0;
}
