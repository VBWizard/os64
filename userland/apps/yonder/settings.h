#ifndef YONDER_SETTINGS_H
#define YONDER_SETTINGS_H

// yonder's Settings window (YONDER.md § Settings), opened by the title
// bar's Settings action: who yonder says it is, picked from the presets or
// typed, and whether pictures and style sheets are kept on disk
// (CACHE.md), with what the cache holds and a button that empties it.
// Apply changes both for this window; Save as default writes them to
// yonder.conf, where the next yonder reads them.
//
// The dialog is a window of its own, and yonder's loop waits on its own
// window alone, so a thread waits on the dialog's and rings `bell` on
// yonder's when something arrives there; the loop answers with
// yonder_settings_rung.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "way/cache.h"

// Opens the dialog over `parent`, showing `agent` and `cache` (NULL: there
// is none), or focuses it if it is open. `use` is handed each agent
// applied, one yonder_agent_valid passed.
void yonder_settings_open(int64_t parent, uint32_t bell, const char *agent,
                          void (*use)(const char *agent), way_cache_t *cache);
// The bell rang: what arrived at the dialog is handled and painted.
void yonder_settings_rung(void);
// Closes the dialog if it is open.
void yonder_settings_close(void);

// The cache yonder.conf asks for — `cache = off`, `cache_dir`, `cache_mb`
// — opened. NULL on no memory.
way_cache_t *yonder_settings_cache_open(void);

// The agent yonder.conf saves, into `out` (YONDER_AGENT_MAX bytes): true
// when there is one and it is valid.
bool yonder_settings_saved_agent(char *out, size_t cap);

// An agent that lives as long as yonder does: a preset's own string, or a
// copy of a typed one. Work already submitted reads the agent it was handed
// when it opens its fetch, which may be after another is applied, so no
// agent is freed before yonder_agents_release, once the workers are gone.
// NULL when there is no memory for the copy.
const char *yonder_agent_keep(const char *agent);
void yonder_agents_release(void);

#endif
