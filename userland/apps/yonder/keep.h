#ifndef YONDER_KEEP_H
#define YONDER_KEEP_H

// KEPT DRAWINGS (PILE3.md § Transforms, the painter: Kept drawings): what a
// group drawn through its matrix painted, its two sheets, kept between
// paints of one tree so a frame that only turns it lays them again instead
// of painting it again. This is the face's store, found by the painter's
// key (paint.h, yonder_keep_key_t) and a stamp of the face's own — what it
// alone knows changed, a picture's arrival among them. Pure: no surface,
// no window, so tools/test_yonder_keep_host.sh drives it under ASan.

#include "paint.h"

typedef struct yonder_keeps yonder_keeps_t;

// At most this many groups are remembered at once, a drawing held or not;
// past it the one asked longest ago is forgotten.
#define YONDER_KEEPS_GROUPS 256

// An empty store holding at most `budget` bytes of sheets; NULL for no
// memory.
yonder_keeps_t *yonder_keeps_new(size_t budget);
void yonder_keeps_free(yonder_keeps_t *k);

// A paint of the tree whose serial is `tree` (flow_tree_serial) begins:
// everything kept for another tree is let go, and what this paint asks for
// is this paint's, which nothing made later in it lets go.
void yonder_keeps_paint(yonder_keeps_t *k, uint64_t tree);

// The answer to the painter's `keep` (paint.h's YONDER_KEEP_*) for `key`,
// the face's stamp being `face`. A group is kept once it has been asked
// for in two paints under the same key and stamp — a frame that only
// turns it asks the same — so a drawing painted once is never painted
// whole for nothing: NO in the first paint (and again in another part of
// it), MAKE in a later one when its sheets fit in the
// budget (letting go of drawings no paint since the last begin asked for,
// least recently asked first), then HAVE while it is held. A key or stamp
// that changed starts it again, its drawing let go. One whose making was
// spoiled (yonder_keeps_spoiled), or that did not fit, is NO until its
// key or stamp changes.
int yonder_keeps_ask(yonder_keeps_t *k, const yonder_keep_key_t *key, uint64_t face);

// The sheets held for `key` and `face` — black and white, key->bounds.w a
// row — or false when none are.
bool yonder_keeps_sheets(yonder_keeps_t *k, const yonder_keep_key_t *key, uint64_t face,
                         const uint32_t **black, const uint32_t **white);

// What a MAKE painted, given to the store: `black` and `white` (each
// key->bounds.w by .h, from os64_malloc) are the store's now, freed when
// let go. False when they no longer fit or the group is no longer asked
// for under that key and stamp: the caller frees them.
bool yonder_keeps_put(yonder_keeps_t *k, const yonder_keep_key_t *key, uint64_t face,
                      uint32_t *black, uint32_t *white);

// A MAKE whose painting may not be kept (paint.h): NO until its key or
// stamp changes.
void yonder_keeps_spoiled(yonder_keeps_t *k, const yonder_keep_key_t *key, uint64_t face);

// Since the counts last restarted: drawings laid from what was kept, made,
// groups seen with a key or stamp they were not kept under, makings
// spoiled, and drawings that did not fit; and what is held now, in bytes
// and drawings, against the budget. All zero for NULL.
typedef struct {
    uint64_t laid, made, seen, spoiled, no_room;
    uint64_t bytes, held, budget;
} yonder_keeps_counts_t;
yonder_keeps_counts_t yonder_keeps_counts(const yonder_keeps_t *k);
void yonder_keeps_counts_restart(yonder_keeps_t *k);

#endif
