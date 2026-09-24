#include <assert.h>
#include <stdlib.h>
#include <string.h>
#define main echo_main
#include "../userland/apps/echo/echo.c"
#undef main
static char output[200000];
static size_t used, max_write;
static bool no_memory, fail_write;
void *os64_malloc(size_t n) { return no_memory ? NULL : malloc(n); }
void os64_free(void *p) { free(p); }
int32_t os64_printf(const char *f, ...) { (void)f; return 0; }
int32_t os64_hprintf(int32_t h, const char *f, ...) { (void)h; (void)f; return 0; }
int64_t os64_write(int32_t h, const void *p, size_t n)
{
    assert(h == 1);
    if (fail_write) return 0;
    if (n > max_write) n = max_write;
    assert(used + n <= sizeof(output));
    memcpy(output + used, p, n); used += n;
    return (int64_t)n;
}
int main(void)
{
    char *text = malloc(OS64_SPAWN_ARG_MAX);
    assert(text);
    memset(text, 'q', OS64_SPAWN_ARG_MAX - 4);
    memcpy(text + OS64_SPAWN_ARG_MAX - 4, "\\nZ", 4);
    char *args[] = {"echo", "-e", text, NULL};
    max_write = 31;
    assert(echo_main(3, args) == 0);
    assert(used == OS64_SPAWN_ARG_MAX - 1);
    for (size_t i = 0; i < OS64_SPAWN_ARG_MAX - 4; i++) assert(output[i] == 'q');
    assert(memcmp(output + used - 3, "\nZ\n", 3) == 0);
    used = 0; no_memory = true;
    assert(echo_main(3, args) == 1 && !used);
    no_memory = false; fail_write = true;
    assert(echo_main(3, args) == 1);
    free(text);
    return 0;
}
