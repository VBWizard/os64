#ifndef YONDER_DOWNLOAD_H
#define YONDER_DOWNLOAD_H

// What a download is called (YONDER_HONEY_DO.md § 6), the name the Save
// dialog proposes, how its size is said, and how fast it goes. Pure — no
// file system, no window — so the host suite drives every rule.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// A proposed name's longest, in bytes: room for a name and its extension,
// well inside a path, and short enough to read in the dialog's field.
#define YONDER_DOWNLOAD_NAME_MAX 128

// The name for a download of `url`, into `out` (at least
// YONDER_DOWNLOAD_NAME_MAX bytes), from the server's `Content-Disposition`
// line when there is one (`disposition`, NULL or "" for none): its
// `filename*` when that is UTF-8, else its `filename`. Without one, the
// last segment of the address's path, percent-decoded. Without that,
// "download". Whatever it came from, a path in it is stripped to its last
// part, a byte the file systems refuse or a terminal would act on (a
// control byte, `/`, `\`) becomes `_`, leading dots go (a server does not
// get to hide a file), and so do trailing dots and spaces; a name left
// empty is "download". A name too long is cut, keeping its extension.
void yonder_download_name(const char *disposition, const char *url, char *out, size_t cap);

// The first of `name`, `name-1`, `name-2`... (the number before the first
// dot past the name's first byte, so `a.tar.gz` becomes `a-1.tar.gz`) for
// which `taken` answers false, into `out`. False when a hundred are taken,
// or the numbered name will not fit `cap`.
bool yonder_download_free_name(const char *name, bool (*taken)(void *ctx, const char *name), void *ctx,
                               char *out, size_t cap);

// `bytes` as a person reads a size, into `out`: kilobytes under a megabyte
// (rounded up, so a byte is never "0 KB"), megabytes and gigabytes to a
// tenth (cut, never rounded up past what there is).
void yonder_download_size(uint64_t bytes, char *out, size_t cap);

// A download's recent pace: how many bytes it had at moments a quarter of a
// second or more apart, the oldest forgotten as new ones come, so the speed
// it gives is the average over the last few seconds, and falls to nothing
// across a stall rather than holding the last speed seen. Zeroed is empty.
#define YONDER_DOWNLOAD_SAMPLES 24
#define YONDER_DOWNLOAD_SAMPLE_MS 250
#define YONDER_DOWNLOAD_WINDOW_MS 5000
typedef struct {
    uint64_t at[YONDER_DOWNLOAD_SAMPLES], got[YONDER_DOWNLOAD_SAMPLES];
    uint32_t count, newest;
} yonder_download_pace_t;

// `got` bytes at `now_ms`, kept unless it comes under
// YONDER_DOWNLOAD_SAMPLE_MS after the last one kept.
void yonder_download_pace_note(yonder_download_pace_t *pace, uint64_t now_ms, uint64_t got);
// Bytes a second from the oldest note of the last YONDER_DOWNLOAD_WINDOW_MS
// to `got` bytes at `now_ms` (from the newest note, when none is that
// recent; from the one before it, when it is under a second old). False
// when that spans less than a second: there is no speed to say yet.
bool yonder_download_pace_rate(const yonder_download_pace_t *pace, uint64_t now_ms, uint64_t got,
                               uint64_t *rate);

// About how long `left` bytes take at `rate` a second, as a person reads
// it, into `out`: "40 s left", "12 min left", "2 h left", "2 h 5 min left",
// rounded up so it never says less than it will be. False when `rate` is 0.
bool yonder_download_left(uint64_t left, uint64_t rate, char *out, size_t cap);

#endif
