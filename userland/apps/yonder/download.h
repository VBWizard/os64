#ifndef YONDER_DOWNLOAD_H
#define YONDER_DOWNLOAD_H

// What a download is called (YONDER_HONEY_DO.md § 6), the name the Save
// dialog proposes, and how its size is said. Pure — no file system, no
// window — so the host suite drives every rule.

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

#endif
