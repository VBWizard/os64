#ifndef YONDER_SETTINGS_H
#define YONDER_SETTINGS_H

// yonder's Settings window (YONDER.md § Settings), opened by the title
// bar's Settings action: who yonder says it is, picked from the presets or
// typed; whether page scripts run, and how long one script task may run;
// whether pictures and sheets stay on disk (CACHE.md), with usage and an
// Empty button; the default zoom; dark pages (`appearance = dark`); and
// whether animations run (`animations = off`) and whether pages are told
// to reduce motion (`motion = reduce`); whether pages get the Modern
// Web arc's surface (`modern = off`), with Clear page storage beside it;
// and whether cookies are taken and kept (`cookies = on | session | off`),
// with Clear cookies beside them. Apply changes this window; Save as
// default writes to yonder.conf.
//
// The dialog is a window of its own, registered with yonder's dispatcher:
// the loop's turns deliver its events and paint it, and it closes itself
// when asked.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cookies.h"
#include "os64/ui_dispatcher.h"
#include "way/cache.h"

// A script task's time limit, in whole seconds (DOM_D7.md § Policy). The
// default is a lean, to be moved by what the P5's audit lines measure.
#define YONDER_SCRIPT_SECONDS_DEFAULT 5
#define YONDER_SCRIPT_SECONDS_MIN 1
#define YONDER_SCRIPT_SECONDS_MAX 60

// Opens with the current choices, or focuses the existing dialog.
// Callbacks receive a validated agent/script policy (the switch and the
// time limit in seconds), zoom (thousandths, whole percentages from 25
// to 500), dark pages, the animations' switches, the modern switch and the
// cookies' mode on the window thread.
void yonder_settings_open(int64_t parent, os64_ui_dispatcher_t *dispatcher, const char *agent,
                          void (*use)(const char *agent, bool scripts, uint32_t script_seconds),
                          way_cache_t *cache, bool scripts, uint32_t script_seconds, uint32_t zoom,
                          void (*zoom_use)(uint32_t thousandths), bool dark,
                          void (*dark_use)(bool dark), bool run, bool reduce,
                          void (*motion_use)(bool run, bool reduce), bool modern,
                          void (*modern_use)(bool modern), yonder_cookies_mode_t cookies,
                          void (*cookies_use)(yonder_cookies_mode_t mode));
// What the dialog's "Clear page storage" does, on the window thread: it
// writes the sentence the dialog's status line then says. Set once, before
// the dialog opens; with none, the button says there is nothing to clear.
void yonder_settings_on_clear_storage(void (*clear)(char *line, size_t cap));
// The same for "Clear cookies"; with none, the button says this window
// keeps no cookies to clear.
void yonder_settings_on_clear_cookies(void (*clear)(char *line, size_t cap));
// Why the cookie file is not kept (NULL while it is), asked on the window
// thread as the dialog opens: the status line says it in place of what the
// cache holds. Set once, before the dialog opens.
void yonder_settings_on_cookies_problem(const char *(*problem)(void));
// Closes the dialog if it is open.
void yonder_settings_close(void);

// The cache yonder.conf asks for — `cache = off`, `cache_dir`, `cache_mb`
// — opened. NULL on no memory.
way_cache_t *yonder_settings_cache_open(void);

// Pages are light unless yonder.conf says `appearance = dark`.
bool yonder_settings_saved_dark(void);
// Animations run unless yonder.conf says `animations = off`.
bool yonder_settings_saved_animations(void);
// Pages are told to reduce motion only when yonder.conf says
// `motion = reduce`.
bool yonder_settings_saved_reduce_motion(void);
// Scripts default off; only the saved literal on enables them.
bool yonder_settings_saved_scripts(void);
// THE MODERN WEB'S SWITCH (MODERN_WEB.md, rule 4): `modern = on | off`.
// Absent it is on; present, it is obeyed. A value that is neither is read
// as on, and `*refused` (may be NULL) says so for the status line;
// otherwise it is NULL.
bool yonder_settings_saved_modern(const char **refused);
// THE COOKIES' SWITCH (cookies.h): `cookies = on | session | off`, without
// case. Absent it is on; a value that is none of the three is read as on,
// and `*refused` (may be NULL) says so for the status line; otherwise it
// is NULL.
yonder_cookies_mode_t yonder_settings_saved_cookies(const char **refused);
// The saved script time limit (`script_seconds = 10`), whole seconds from
// YONDER_SCRIPT_SECONDS_MIN to _MAX; the default when absent or not one.
uint32_t yonder_settings_saved_script_seconds(void);

// The directory yonder.conf names for page files (`diagnostics =
// /home/yonder/pages`, YONDER_DIAGNOSTICS.md), into `out`, without a
// trailing slash: true when there is one and it is a full path.
bool yonder_settings_saved_diagnostics(char *out, size_t cap);
// Whether yonder.conf asks for each page to be kept beside its page file
// (`keep_pages = on`, MODERN_WEB.md rule 3: its bytes, scripts and sheets,
// for the replay corpus). Off when absent; a value that is neither `on` nor
// `off` is logged and read as off. It keeps nothing without `diagnostics`.
bool yonder_settings_saved_keep_pages(void);

// THE PROFILE FOLDER (YONDER_HONEY_DO.md § Where yonder's files live):
// `profile = <dir>`, where the person's own files are kept, page storage
// and cookies among them. Into `out` without a trailing slash; absent, it is
// YONDER_PROFILE_DEFAULT. A value that is not a full path, or too long for
// `out`, or a yonder.conf that cannot be read, is refused for the default,
// and the answer is then the sentence for the status line (NULL
// otherwise). The folder is made when something is first kept in it.
#define YONDER_PROFILE_DEFAULT "/home/yogi/.yonder"
const char *yonder_settings_saved_profile(char *out, size_t cap);

// THE DOWNLOADS FOLDER, by the profile's rules: `downloads = <dir>`, where
// the Save dialog opens. The profile's sibling and not its child: a
// download is something the person asked for and will look for. Made when
// the first download is asked for.
#define YONDER_DOWNLOADS_DEFAULT "/home/yogi/downloads"
const char *yonder_settings_saved_downloads(char *out, size_t cap);

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
