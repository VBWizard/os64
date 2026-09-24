#include "os64/os64.h"
#include "os64/procfs.h"

int32_t os64_proc_command(uint64_t pid, char **out)
{
    if (!out) return -1;
    *out = NULL;
    char path[96];
    os64_snprintf(path, sizeof(path), "/proc/%lu/cmdline", pid);
    int32_t handle = (int32_t)os64_open(path, NULL);
    if (handle < 0) return -1;

    const size_t limit = 1024 * 1024;
    size_t capacity = 512, used = 0;
    char *text = os64_malloc(capacity + 1);
    bool failed = text == NULL;
    while (!failed)
    {
        if (used == capacity)
        {
            // Probe before growing so an exact-capacity file needs no extra
            // allocation, and an oversized report is refused, not truncated.
            char byte;
            int64_t n = os64_read(handle, &byte, 1);
            if (!n) break;
            if (n < 0 || capacity == limit) { failed = true; break; }
            size_t next = capacity * 2;
            char *grown = os64_realloc(text, next + 1);
            if (!grown) { failed = true; break; }
            text = grown;
            capacity = next;
            text[used++] = byte;
        }
        int64_t n = os64_read(handle, text + used, capacity - used);
        if (n < 0) { failed = true; break; }
        if (!n) break;
        used += (size_t)n;
    }
    if (os64_close(handle) < 0) failed = true;
    if (failed)
    {
        os64_free(text);
        return -1;
    }
    if (used && text[used - 1] == '\n') used--;
    for (size_t i = 0; i < used; i++)
        if (text[i] == '\n') text[i] = ' ';
    text[used] = '\0';
    *out = text;
    return 0;
}
