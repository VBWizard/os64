#ifndef YONDER_SETTINGS_H
#define YONDER_SETTINGS_H

// yonder's Settings window (YONDER.md § Settings), opened by the title
// bar's Settings action: who yonder says it is, picked from the presets or
// typed; whether page scripts run, and how long one script task may run;
// whether pictures and sheets stay on disk (CACHE.md), with usage and an
// Empty button; the default zoom; and dark pages (`appearance = dark`).
// Apply changes this window; Save as default writes to yonder.conf.
//
// The dialog is a window of its own, and yonder's loop waits on its own
// window alone, so a thread waits on the dialog's and rings `bell` on
// yonder's when something arrives there; the loop answers with
// yonder_settings_rung.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "way/cache.h"

// A script task's time limit, in whole seconds (DOM_D7.md § Policy). The
// default is a lean, to be moved by what the P5's audit lines measure.
#define YONDER_SCRIPT_SECONDS_DEFAULT 5
#define YONDER_SCRIPT_SECONDS_MIN 1
#define YONDER_SCRIPT_SECONDS_MAX 60

// Opens with the current choices, or focuses the existing dialog.
// Callbacks receive a validated agent/script policy (the switch and the
// time limit in seconds) and zoom (thousandths, whole percentages from 25
// to 500) and dark pages on the window thread.
void yonder_settings_open(int64_t parent, uint32_t bell, const char *agent,
                          void (*use)(const char *agent, bool scripts, uint32_t script_seconds),
                          way_cache_t *cache, bool scripts, uint32_t script_seconds, uint32_t zoom,
                          void (*zoom_use)(uint32_t thousandths), bool dark,
                          void (*dark_use)(bool dark));
// The bell rang: what arrived at the dialog is handled and painted.
void yonder_settings_rung(void);
// Closes the dialog if it is open.
void yonder_settings_close(void);

// The cache yonder.conf asks for — `cache = off`, `cache_dir`, `cache_mb`
// — opened. NULL on no memory.
way_cache_t *yonder_settings_cache_open(void);

// Pages are light unless yonder.conf says `appearance = dark`.
bool yonder_settings_saved_dark(void);
// Scripts default off; only the saved literal on enables them.
bool yonder_settings_saved_scripts(void);
// The saved script time limit (`script_seconds = 10`), whole seconds from
// YONDER_SCRIPT_SECONDS_MIN to _MAX; the default when absent or not one.
uint32_t yonder_settings_saved_script_seconds(void);

// The directory yonder.conf names for page files (`diagnostics =
// /home/yonder/pages`, YONDER_DIAGNOSTICS.md), into `out`, without a
// trailing slash: true when there is one and it is a full path.
bool yonder_settings_saved_diagnostics(char *out, size_t cap);

// The agent yonder.conf saves, into `out` (YONDER_AGENT_MAX bytes): true
// when there is one and it is valid.
bool yonder_settings_saved_agent(char *out, size_t cap);
// The zoom yonder.conf saves (`zoom = 125`, a whole percent), in
// thousandths; 1000 when there is none or it is not one from 25 to 500.
uint32_t yonder_settings_saved_zoom(void);

// An agent that lives as long as yonder does: a preset's own string, or a
// copy of a typed one. Work already submitted reads the agent it was handed
// when it opens its fetch, which may be after another is applied, so no
// agent is freed before yonder_agents_release, once the workers are gone.
// NULL when there is no memory for the copy.
const char *yonder_agent_keep(const char *agent);
void yonder_agents_release(void);

#endif
