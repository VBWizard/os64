#ifndef APPEARANCE_H
#define APPEARANCE_H
#include "os64/appearance.h"

// Called from kernel thread context with kernel-owned buffers, no GUI lock held.
// Snapshot copies into caller storage. Publication and window notification
// share kGuiLock so no reader can observe a generation before it is announced.
int appearance_snapshot(char *out, size_t cap);
int appearance_publish(const char *bytes, size_t length);
size_t appearance_length(void);
#endif
