#ifndef YONDER_STORAGE_H
#define YONDER_STORAGE_H

// WHERE A PAGE'S STORAGE IS KEPT (MODERN_WEB.md § Slice 1a): libdom holds
// the map a page sees and says what the page changed; this file moves the
// bytes. localStorage is a file per origin, `<profile>/storage/<origin>`,
// and the FILE is the state every window of every yonder shares: each
// commit reads it as it stands, merges this page's changes onto it
// (os64_dom_storage_merge) and publishes the result by temp and rename, so
// two windows lose nothing of each other's but the same key written by
// both, where the later commit wins, unless they commit in the same
// instant (storage.c § Two yonders, one store). sessionStorage is this
// window's alone:
// an image per origin in memory, kept across the window's navigations and
// gone with the window, never a file.
//
// Images are libdom's format; this file moves them, and takes one apart
// only through libdom's merge and walk.

#include <dom/dom.h>

#include "diag.h"

typedef struct yonder_storage yonder_storage_t;

// The window's storage over the profile folder `profile` (a full path,
// copied): nothing is made on disk until a page commits. NULL on no memory.
yonder_storage_t *yonder_storage_new(const char *profile);
void yonder_storage_free(yonder_storage_t *storage);

// The provider's three verbs (dom.h § os64_dom_storage_t), for `area`.
// identity: localStorage's is the file's size and modification time, 0
// when there is no file; sessionStorage's counts the window's commits to
// that origin. load: the image into `out` when it fits in `cap`; its length
// either way, 0 for none, negative when the file is there and cannot be
// read or the origin names no file. commit: the change list merged onto
// the store as it stands, published by temp and rename. A merge past the
// quota keeps nothing and names, in `diag` (may be NULL), the keys the
// commit could not keep. The first touch of the profile's stores removes
// the temps a crash left.
uint64_t yonder_storage_identity(yonder_storage_t *storage, os64_dom_storage_area_t area, const char *origin);
int64_t yonder_storage_load(yonder_storage_t *storage, os64_dom_storage_area_t area, const char *origin,
                            void *out, size_t cap);
os64_dom_storage_status_t yonder_storage_commit(yonder_storage_t *storage, os64_dom_storage_area_t area,
                                                const char *origin, const void *changes, size_t length,
                                                yonder_diag_t *diag);

// Settings' "Clear page storage": every store under <profile>/storage/ is
// removed, one unlink each, with every temp a crash left (a live window's,
// mid-commit, stays, and that window may then publish the store as it read
// it before the clear, with its changes), and this window's sessionStorage
// forgotten. The pages open now
// forget their own maps with os64_dom_storage_forget. Answers how many
// stores went, negative when the folder could not be listed or a file
// could not be removed (what could be, was).
int64_t yonder_storage_clear(yonder_storage_t *storage);

// Why localStorage is kept in no file, when it is not: the profile folder
// could not be made, a store could not be read or written or is not
// libdom's, or an origin's address is too long to name a file. The latest
// such answer, until Clear page storage; NULL while nothing has gone
// wrong. For the status line.
const char *yonder_storage_problem(const yonder_storage_t *storage);

// ── The profile's other files (the cookie file, cookies.c) ──────────────
//
// The same three rules page storage keeps for its own, for a file of the
// profile's: its folders made when it is first written, a write published
// by a temp beside it and a rename, and the temps a crash left removed.

// The room a temp's name takes past its file's path: a full path shorter
// than OS64_PATH_MAX by this much has a temp.
#define YONDER_PROFILE_TEMP_SUFFIX_MAX (1 + 20 + 1 + 10 + 4)
// Every folder of `path` that is missing, made, parents first. False when
// one cannot be, or a name on the way is a file.
bool yonder_profile_make_dirs(const char *path);
// `bytes` written whole and synced to a temp beside `path`, named into
// `temp` with the next of `*count`'s numbers, for a rename to publish.
// False with no temp left behind.
bool yonder_profile_temp_write(uint32_t *count, const char *path, const void *bytes, size_t length, char *temp,
                               size_t cap);
// `path`'s temps whose writer is gone (another yonder's that died, or this
// one's), removed. A live yonder's temp stays.
void yonder_profile_sweep(const char *path);

// An origin as a file name: every byte outside [A-Za-z0-9.-] as %XX, so
// `https://example.com:8443` is `https%3A%2F%2Fexample.com%3A8443`. False
// when it does not fit `cap`.
bool yonder_storage_file_name(const char *origin, char *out, size_t cap);

#endif
