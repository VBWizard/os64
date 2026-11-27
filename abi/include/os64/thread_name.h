#ifndef OS64_THREAD_NAME_H
#define OS64_THREAD_NAME_H

// A thread's name (docs/design/pending/THREAD_NAMES.md): given when the thread
// is made, as the thread syscall's fourth argument (libos64's
// os64_thread_named), fixed for the thread's life, and shown as the `name`
// line of /proc/<id>/thread/<tid>/status. It describes; it identifies
// nothing: two threads may share one, and nothing looks a thread up by it.

#include <stdbool.h>

// Bytes of a name, its terminator included: at most 63 name bytes.
#define OS64_THREAD_NAME_MAX 64

// Does the kernel take `name`? It must end within OS64_THREAD_NAME_MAX bytes
// and every byte before its end must be printable ASCII, 0x20 to 0x7E, so a
// status line and a monitor's row stay literal and bounded. The empty name is
// taken and means unnamed. A name that breaks either rule is refused, never
// cut short.
static inline bool os64_thread_name_valid(const char *name)
{
    for (int i = 0; i < OS64_THREAD_NAME_MAX; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c == '\0')
            return true;
        if (c < 0x20 || c > 0x7E)
            return false;
    }
    return false;
}

#endif
