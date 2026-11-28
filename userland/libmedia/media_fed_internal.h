#ifndef MEDIA_FED_INTERNAL_H
#define MEDIA_FED_INTERNAL_H
// The fed media's side facing libmedia's decoding (media.c), not exported.
// Everything here is the decoding thread's, and every pointer it hands out
// is good only while the fed lock is held: a buffer may be removed, and its
// track buffer freed, as soon as the lock is let go.
#include "os64/bell.h"
#include "os64/media_fed.h"
#include "os64/media_track.h"

// Bytes of zeros after every block the fed media's memory functions give:
// FFmpeg's AV_INPUT_BUFFER_PADDING_SIZE (media.c asserts the two agree), so
// a decoder reading past the last frame of a slab reads zeros it owns.
#define MEDIA_FED_PADDING 64

// The decoding thread's hold (the player's): taken at the player's create,
// let go at its finish; the fed media is freed by whichever of the two
// holders lets go last. The bell, NULL for none, is rung whenever what a
// cursor could read changes (a parse that added frames or an init, a
// removal, the end, a buffer removed, the close).
void media_fed_hold(os64_media_fed_t *fed, os64_bell_t *bell);
void media_fed_release(os64_media_fed_t *fed);

void media_fed_lock(os64_media_fed_t *fed);
// Lets go of the lock, then frees what was released under it.
void media_fed_unlock(os64_media_fed_t *fed);

// Under the lock. Whether the tracks are picked: the first time every
// buffer holds an initialization segment, the first picture track among
// them and the first sound track. False until then (or with no buffers).
bool media_fed_picked(os64_media_fed_t *fed);
// Under the lock: the picked track of a kind (OS64_MEDIA_FRAGMENT_VIDEO or
// _AUDIO), or NULL when there is none, or its buffer was removed, or the
// creator closed the fed media. `changes` (may be NULL) is set to a count
// that moves whenever a buffer is removed, so a cursor into one is known
// to be stale.
os64_media_track_t *media_fed_track(os64_media_fed_t *fed, int kind, uint64_t *changes);
// Under the lock: endOfStream() was called and nothing has reopened it.
bool media_fed_ended(const os64_media_fed_t *fed);
// Under the lock: the duration (NaN unknown, INFINITY none).
double media_fed_duration(const os64_media_fed_t *fed);
// Under the lock: the counts the decoding side keeps.
void media_fed_count_resync(os64_media_fed_t *fed);
void media_fed_count_sound(os64_media_fed_t *fed, uint64_t silence_frames, uint64_t cut_frames);
void media_fed_count_reopen(os64_media_fed_t *fed);

#endif
