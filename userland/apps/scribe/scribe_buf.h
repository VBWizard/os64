// scribe_buf.h — scribe's text model: an array of independently-allocated
// lines behind libui's textbuf vtable. Boring on purpose (SCRIBE.md: not a
// gap buffer, not a rope) — the os64 files that matter fit in RAM, and the
// three mutation choke points are shaped so slice two's undo journal can
// attach without a rewrite.

#ifndef SCRIBE_BUF_H
#define SCRIBE_BUF_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "os64/ui.h"

typedef struct
{
    char  *bytes;   // NOT NUL-terminated — length-counted, like the file
    size_t len;
    size_t cap;
} sbuf_line_t;

typedef struct
{
    sbuf_line_t *lines;
    size_t count;
    size_t cap;
    bool dirty;     // unsaved edits exist (set by the vtable, cleared by save)
} sbuf_t;

// Start life as one empty line — a buffer with zero lines has no place to
// put a cursor, so the invariant is count >= 1, forever.
bool sbuf_init(sbuf_t *b);
void sbuf_free(sbuf_t *b);

// Load `path` whole. Returns:
//   0  — loaded
//   1  — no such file: buffer reset to empty (the "new file" answer)
//  -1  — refused or failed; `err` says why, with numbers when it's memory
int sbuf_load(sbuf_t *b, const char *path, char *err, size_t errcap);

// Write every line back, newline-terminated (a final line that arrived
// without one gains one — the Unix convention, applied on the way out).
// 0 on success, -1 with `err` filled on failure. Clears `dirty` on success.
int sbuf_save(sbuf_t *b, const char *path, char *err, size_t errcap);

// The vtable template: copy it, point `user` at your sbuf_t, hand the copy
// to ui_textview. (A template rather than a global-with-one-user, so two
// buffers in one future scribe cost nothing new.)
extern const os64_ui_textbuf_t sbuf_textbuf_template;

#endif // SCRIBE_BUF_H
