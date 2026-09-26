#ifndef GARB_INTERNAL_H
#define GARB_INTERNAL_H

// libgarb's private seam between the tokenizer, the parser, the decoder and
// the dumps. Section numbers are CSS Syntax Level 3's.

#include "garb/garb.h"
#include "os64/arena.h"

// What one parse may take, past the input itself: a 2026 page's biggest
// sheet parses in a few MiB, and a result past this is cut short and
// marked incomplete rather than allowed to eat the machine.
#define GARB_ARENA_MAX ((size_t)96 << 20)

// ── The tokenizer (§4) ──────────────────────────────────────────────────

// The tokens the parser sees. A finished component value (CV) is what a
// list of component values yields when the parser reads one instead of
// text — §5's "token stream" may be either.
typedef enum {
    T_VALUE,                // `v` holds a token that is also a component value
    T_FUNCTION,             // `v.text` is the name; arguments follow
    T_OPEN,                 // '{', '[' or '(' in `v.open`
    T_CLOSE,                // '}', ']' or ')' in `v.open`
    T_EOF,
} tok_kind_t;

typedef struct {
    tok_kind_t kind;
    garb_value_t v;
} Tok;

typedef struct {
    uint32_t *cp;           // the input, preprocessed (§3.3), as code points
    size_t n, pos;
    os64_arena_t *arena;    // where token text goes
    bool short_of_memory;
    // A scratch buffer for building UTF-8 before it is copied into the arena.
    char *buf;
    size_t buflen, bufcap;
} Tokenizer;

// Preprocesses UTF-8 text into code points. False on no memory.
bool tz_open(Tokenizer *tz, const char *text, size_t len, os64_arena_t *arena);
void tz_close(Tokenizer *tz);
Tok tz_next(Tokenizer *tz);

// ── The parser's input: text, or component values already parsed ───────

typedef struct {
    Tokenizer *tz;          // or:
    const garb_value_t *values;
    int32_t n, at;
    Tok peeked;
    bool have_peek;
} Input;

// ── A parse's arena and its state ───────────────────────────────────────

typedef struct {
    os64_arena_t *arena;
    bool incomplete;
    int32_t depth;
} Parse;

// Appending to a list held in the arena: the list doubles, and a list that
// cannot grow is left as it was and the parse marked incomplete.
bool p_push(Parse *p, void **list, int32_t *n, int32_t *cap, size_t size, const void *item);

#endif
