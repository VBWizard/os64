#ifndef YONDER_MEDIA_BYTES_H
#define YONDER_MEDIA_BYTES_H

// The arithmetic of a sound's bytes as they come off the network (media.c),
// pure so the host can drive it (tools/test_media_bytes_host.sh): which of a
// seek's answers to give, how much of the kept bytes to let go for room,
// what a response's head says about its length, its ranges and its type,
// and the words a failed element is recorded in.
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
// Content-Type's media type, without its parameters (`text/html` of
// `text/html; charset=utf-8`), into `out`: the last one the head holds;
// empty when there is none, or when that one does not fit.
void yonder_media_content_type(const char *headers, size_t length, char *out, size_t cap);

// What a failed element is recorded and said with (yonder.c's FAILED
// media line): the element, the error the page was handed, the reason
// handed with it, and what is known of how it came to that. Strings are
// never NULL; "" is none.
typedef struct {
    const char *element;        // "video 2": its kind and its place among the page's own
    int error;                  // MediaError's code (dom/media.h)
    const char *reason;         // MediaError.message
    bool fetched;               // from the network (else a file)
    // The last request made, which a fetch's failure is the failure of:
    int status;                 // its answer's HTTP status, 0 when no head came
    const char *type;           // that answer's Content-Type
    uint64_t received;          // the bytes it brought
    uint64_t range_from;        // its first byte: 0 the first request, else a Range request
    const char *said;           // libmedia's own words for a failed open or decode
    const char *format;         // the container libmedia read it as
    const char *sound, *picture;    // the codecs it named
    bool sound_unplayable, picture_unplayable;
} yonder_media_failure_t;

// The failure in words, into `out` (cut short to fit, always ended):
// `video 2, not supported: The server answered 403 (HTTP 403, text/html,
// 0 bytes received, the first request)`, and after a `;` what libmedia
// said, when it is not the reason, and what it named.
void yonder_media_failure_text(const yonder_media_failure_t *f, char *out, size_t cap);

#endif
