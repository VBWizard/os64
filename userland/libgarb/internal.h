#ifndef GARB_INTERNAL_H
#define GARB_INTERNAL_H

// libgarb's private seam between the tokenizer, the parser, the decoder and
// the dumps. Section numbers are CSS Syntax Level 3's.

#include "garb/garb.h"
#include "os64/arena.h"

// What one parse may take, past the input itself: about 5 MiB of ordinary
// CSS with every block read, far less of the densest (GARB.md § The cost,
// measured). A result past this is cut short and marked incomplete rather
// than allowed to eat the machine.
#define GARB_ARENA_MAX ((size_t)96 << 20)

// ── Room for what is open ───────────────────────────────────────────────
//
// THE ARENA ALWAYS HAS ROOM TO KEEP WHAT IS OPEN. parse.c's lists reach the
// arena only when they close (parse.c § The lists a parse builds), so an
// arena filled by anything else would lose every list still open — the
// sheet's own list of rules among them, the day a sheet reached the cap.
// Every allocation but a close — a token's text, a rule, a push onto an
// open list — asks garb_room first: what the arena has reserved, the
// request, and `hold` (the open lists' bytes, plus GARB_CLOSE_SLACK) must
// fit. Filling the arena then refuses a token or a rule, never the lists
// that hold what was finished.
//
// The slack is for how an arena reserves: in chunks (os64/arena.h — they
// grow to 256 KiB), so one small request can reserve a whole chunk, and a
// close bigger than what is left of the current chunk starts one of its
// own and strands that tail. Two chunks' worth covers both; and a chunk
// header and alignment for every list that could be open.
#define GARB_CLOSE_SLACK ((size_t)2 * ((256u << 10) + 64) + (size_t)(GARB_DEPTH_MAX * 4 + 8) * 128)

static inline bool garb_room(const os64_arena_t *arena, size_t size, size_t hold)
{
    size_t reserved = os64_arena_stats(arena).reserved_bytes;
    return reserved <= GARB_ARENA_MAX && size <= GARB_ARENA_MAX - reserved &&
           hold <= GARB_ARENA_MAX - reserved - size;
}

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
    const size_t *open;     // the parse's open lists, which text must leave room for
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

// ── A parse's arena and its state ───────────────────────────────────────
//
// THE FIRST ALLOCATION THAT FAILS ENDS THE PARSE, whether it is the arena's
// or the tokenizer's scratch buffer: from then the input reads as its end
// and nothing more is pushed into any list, so whatever was being built —
// a token cut short, a declaration missing a value, the rule round it — is
// never published. What comes back was finished before the failure: real,
// and less of it. (Depth is a different stop: past GARB_DEPTH_MAX contents
// are dropped by design and the parse goes on.)

typedef struct {
    os64_arena_t *arena;
    bool incomplete;
    bool spent;             // an allocation failed: nothing more is read or kept
    Tokenizer *tz;          // the text's tokenizer, whose buffer can fail too
    int32_t depth;
    // Where parse.c's lists are built before they are kept (parse.c § Lists):
    // a stack on the heap, the open lists nested in it, `top` its end.
    char *scratch;
    size_t top, scratch_cap;
} Parse;

static inline bool p_spent(const Parse *p)
{
    return p->spent || (p->tz != NULL && p->tz->short_of_memory);
}

typedef struct {
    Tokenizer *tz;          // or:
    const garb_value_t *values;
    int32_t n, at;
    Tok peeked;
    bool have_peek;
    const Parse *owner;     // spent: the input has ended
} Input;

// Appending to a list held in the arena: the list doubles, and the arena
// never takes the old half back, so this is for lists that stay small (a
// selector's); parse.c builds its own on the scratch stack instead. Once
// the parse is spent nothing is appended, and a list that cannot grow
// spends it.
bool p_push(Parse *p, void **list, int32_t *n, int32_t *cap, size_t size, const void *item);

#endif
