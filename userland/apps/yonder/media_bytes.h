#ifndef YONDER_MEDIA_BYTES_H
#define YONDER_MEDIA_BYTES_H

// The arithmetic of a sound's bytes as they come off the network (media.c),
// pure so the host can drive it (tools/test_media_bytes_host.sh): which of a
// seek's answers to give, how much of the kept bytes to let go for room, and
// what a response's head says about its length and its ranges.
//
// The bytes kept are a WINDOW of the resource: offsets [base, base + have).
// It starts at 0 and grows as the body arrives; a seek far from it starts a
// new one where the seek lands (a Range request), and a resource longer
// than the most kept slides it forward behind the reader.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t base;      // the resource offset of the first byte kept
    uint64_t have;      // bytes kept from there
} yonder_media_window_t;

typedef enum {
    YONDER_MEDIA_SEEK_HERE,     // inside the window: the read goes on from there
    YONDER_MEDIA_SEEK_WAIT,     // a little past it: the reader moves there and its read waits
                                // for the fetch bringing it, or resumes a held one
    YONDER_MEDIA_SEEK_JUMP,     // a new window at the offset (a Range request)
    YONDER_MEDIA_SEEK_REFUSE,   // the reader carries on without it
} yonder_media_seek_t;

// How far past the window a seek waits for the fetch rather than starting
// another: a request costs a round trip or a handshake, a quarter of a
// mebibyte a moment on any line that plays sound.
#define YONDER_MEDIA_JUMP_GAP ((uint64_t)256 * 1024)

typedef struct {
    bool opening;       // the media is being opened: its reader probes and must not wait
    bool ranges;        // the server answers Range requests
    bool fetching;      // a fetch is bringing the bytes at the window's end
    bool held;          // none is, but one will be asked from the window's end when read there
    bool ended;         // the resource's end is in the window
} yonder_media_seek_state_t;

// The answer to a seek to `offset`.
yonder_media_seek_t yonder_media_seek_plan(const yonder_media_window_t *w, uint64_t offset,
                                           const yonder_media_seek_state_t *s);

// How many bytes to let go of from the window's front so `n` more fit in
// `max`, keeping at least `behind` before the reader at `at` (a seek back a
// little is common, and free while the bytes are kept). 0 when they fit;
// UINT64_MAX when even that would not make room.
uint64_t yonder_media_drop_for(const yonder_media_window_t *w, uint64_t n, uint64_t at, uint64_t max,
                               uint64_t behind);

// Whether to take more from the fetch: fewer than `ahead` bytes are kept
// past the reader. The rest waits in the fetch's pipe, which then holds
// the fetch off (its backpressure), so a long sound is never read far
// ahead of the ear.
bool yonder_media_take_more(const yonder_media_window_t *w, uint64_t at, uint64_t ahead);

// A response's head (a header block, lines ending "\n" or "\r\n"):
// Content-Length, or -1 (none, not a number, or a Content-Encoding, whose
// decoded body has no size here).
int64_t yonder_media_content_length(const char *headers, size_t length);
// Content-Range's `bytes FIRST-LAST/TOTAL`: true with the first offset and
// the total (-1 for `*`) when the header is one.
bool yonder_media_content_range(const char *headers, size_t length, uint64_t *first, int64_t *total);
// Whether Accept-Ranges names `bytes`.
bool yonder_media_accepts_ranges(const char *headers, size_t length);
// What says which version of the resource an answer is: its ETag, else
// its Last-Modified (prefixed so the two never compare equal), into `out`;
// empty when it carries neither, or when it does not fit.
void yonder_media_validator(const char *headers, size_t length, char *out, size_t cap);

#endif
