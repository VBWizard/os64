// keys.h — the window's media-key session (os64/mediakeys.h; AUDIO_YONDER.md
// § Y.6). One yonder is one window, so it holds at most one session: opened
// when its page first has something to say, kept for the window's life.
// Everything here but the reader thread is the window thread's.

#ifndef YONDER_KEYS_H
#define YONDER_KEYS_H

#include <stdbool.h>
#include <stdint.h>

#include "dom/media.h"

// Open the session and start its reader, which calls `bell` (any thread)
// after queueing each press. False, and nothing held, when it would not
// open (every session the machine keeps is taken): presses then go to
// other programs, as if this window had none.
bool yonder_keys_open(void (*bell)(void));
bool yonder_keys_opened(void);
// Say the window's state: written to the session only when it changed.
void yonder_keys_say(os64_dom_media_session_state_t state);
// The next press the reader queued (OS64_MEDIA_KEY_*): true and it, or
// false when none waits.
bool yonder_keys_take(uint32_t *key);

#endif
