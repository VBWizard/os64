#ifndef OS64_DECORATION_STARTUP_H
#define OS64_DECORATION_STARTUP_H
#include <stddef.h>

enum {
    OS64_DECOR_STARTUP_DEFAULT=0, OS64_DECOR_STARTUP_APPLIED=1,
    OS64_DECOR_STARTUP_SKIPPED=2, OS64_DECOR_STARTUP_ABSENT=3,
    OS64_DECOR_STARTUP_INVALID=-1, OS64_DECOR_STARTUP_IO=-2,
    OS64_DECOR_STARTUP_MEMORY=-3
};
/* Atomically store a validated, self-contained decoration for the next boot.
 * NULL/0 writes an explicit built-in choice, masking lower config layers.
 * This does not publish to the running window manager. Returns zero on success. */
int os64_decor_startup_save(const void *bytes,size_t length);
/* Called by the desktop before spawning configured applications. Publication
 * expects generation zero, so an explicit Apply that wins the race is retained.
 * An absent/default choice or any failure leaves the live decoration alone. */
int os64_decor_startup_install(void);
#endif
