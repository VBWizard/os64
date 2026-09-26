#ifndef WAY_INTERNAL_H
#define WAY_INTERNAL_H

// The seam between libway's two halves.

#include "way/way.h"

// Sets session->status.
void way_say(way_session_t *session, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

// Whether an address can be offered inside single quotes as a command.
bool way_shell_quotable(const char *address);

#endif
