/* Exercise the target-built private runtime, including its compiler-emitted
 * memory aliases. The same object is used by fonttest and the freestanding
 * host launcher; no hosted macro or libc substitute enters this check. */
#include "../../libfreetype/port/runtime.h"
#include <stdint.h>

static int compare_int(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

int os64_font_runtime_test(void)
{
    unsigned char source[1024], dest[1024];
    volatile size_t length = sizeof(source);
    for (size_t i = 0; i < length; i++) source[i] = (unsigned char)(i * 13u);
    if (ftport_memcpy(dest, source, length) != dest) return 1;
    for (size_t i = 0; i < length; i++) if (dest[i] != source[i]) return 2;
    ftport_memmove(dest + 7, dest, length - 7);
    for (size_t i = 7; i < length; i++) if (dest[i] != source[i - 7]) return 3;
    ftport_memmove(dest, dest + 7, length - 7);
    if (ftport_memcmp(dest, source, length - 7)) return 4;
    ftport_memset(dest, 0xa5, length);
    for (size_t i = 0; i < length; i++) if (dest[i] != 0xa5) return 5;
    int values[] = {8, -1, 8, 0, -7, 3};
    const int sorted[] = {-7, -1, 0, 3, 8, 8};
    ftport_qsort(values, 6, sizeof(values[0]), compare_int);
    for (size_t i = 0; i < 6; i++) if (values[i] != sorted[i]) return 6;
    ftport_jmp_buf jump;
    volatile int marker = 0;
    int answer = ftport_setjmp(jump);
    if (answer == 0) {
        marker = 19;
        ftport_longjmp(jump, 0);
    }
    if (answer != 1 || marker != 19) return 7;
    return 0;
}
