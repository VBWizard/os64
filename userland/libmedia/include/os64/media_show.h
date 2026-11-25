#ifndef OS64_MEDIA_SHOW_H
#define OS64_MEDIA_SHOW_H

// os64/media_show.h — what a player's show thread does with its plane at
// each pass (libmedia.so; docs/design/pending/VIDEO_PLANE.md § 4).
//
// The show thread waits for its plane's frame, picks the picture due, and
// then asks this core what to do. Pure: no locks, no syscalls, no clock of
// its own. The thread carries out what it says, outside the player's lock,
// and tells it what came of it (os64_media_show_published,
// os64_media_show_recreated).
//
// THE RULES (§ 4.1, § 4.3, § 4.4):
// - A PUBLISH IS OWED, placed: for every newly picked picture of the
//   plane's picture size. Not placed: for the first picture the plane is
//   given, then for every newly picked picture while the caller WANTS the
//   plane, and for the current pick after a new size (step 4). Neither:
//   none; the pictures go by the clock.
// - A pick of another size while placed is not published (the kernel keeps
//   a placed plane's size): the plane is STALE until the caller hides it.
// - Not placed, a pick beyond the plane's capacity asks for a new plane
//   (RECREATE) before its publish.
// - READY: the plane holds a picture of the size the player converts to.
// - CLOSING: the first pass that sees it starts a bound; the place empty,
//   the plane is destroyed and the thread ends; still placed when the bound
//   has passed, the thread ends with it placed and the kernel's exit path
//   takes it out (hidden, then destroyed), so the player always finishes.
// - GONE (the window or the plane is no more): the thread ends; there is
//   nothing to destroy.
// - LOOK (the caller's bell) when stale or ready changes.

#include <stdbool.h>
#include <stdint.h>

#pragma GCC visibility push(default)

// How long a closing player waits for its caller to hide the plane.
#define OS64_MEDIA_SHOW_CLOSE_BOUND_US 5000000

typedef struct {
    uint32_t pic_w, pic_h;          // the picture the plane holds (0 x 0 before its first)
    uint32_t cap_w, cap_h;          // the plane's capacity
    uint32_t published;             // the serial of the pick last published, 0 none
    bool stale, ready;
    bool closing;
    int64_t closing_since;
    uint64_t publishes, recreates, ended_placed;
} os64_media_show_t;

typedef struct {
    int64_t now;
    bool placed;                    // the plane's committed state, as the last wait reported it
    bool wanted;                    // the caller means to place it
    bool closing;
    bool gone;
    bool have;                      // a pick to consider
    uint32_t w, h, serial;          // the pick's size and serial
} os64_media_show_input_t;

typedef struct {
    bool publish;                   // copy the pick and publish w x h
    bool recreate;                  // first: a new plane holding w x h (the old one destroyed)
    bool destroy;                   // destroy the plane, then end
    bool end;                       // end the thread (after `destroy`, if set)
    bool look;                      // ring the caller: stale or ready moved
} os64_media_show_act_t;

void os64_media_show_init(os64_media_show_t *s, uint32_t cap_w, uint32_t cap_h);
os64_media_show_act_t os64_media_show_decide(os64_media_show_t *s, const os64_media_show_input_t *in);
// What came of a publish the decision asked for: done (the plane holds the
// pick) or refused (it does not). True when stale or ready moved: ring the
// caller.
bool os64_media_show_published(os64_media_show_t *s, bool done, uint32_t w, uint32_t h, uint32_t serial);
// What came of a recreate: the new plane's capacity, or refused (cap 0, and
// the show thread hands the pace back and ends: os64/media_player.h).
void os64_media_show_recreated(os64_media_show_t *s, uint32_t cap_w, uint32_t cap_h);

#pragma GCC visibility pop

#endif // OS64_MEDIA_SHOW_H
