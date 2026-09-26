#ifndef GARB_GARB_H
#define GARB_GARB_H

// libgarb — the page's garb: CSS, parsed and cascaded (GARB.md). This
// header is the PARSER's half, CSS Syntax Level 3: bytes in, a sheet of
// rules, declarations and component values out, recovering from errors
// exactly as the specification writes it. Pure: no I/O, no clock.
//
// EVERYTHING A PARSE RETURNS LIVES IN ONE ARENA and goes with one free,
// so a pointer into a result is good until that result is freed and not a
// moment longer. Strings are UTF-8 with a NUL after them, and `len` is their
// length in bytes; no string holds a NUL of its own, since the tokenizer
// makes every NUL a U+FFFD, escaped or not.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#pragma GCC visibility push(default)

// ── Component values (§4, §5) ───────────────────────────────────────────

typedef enum {
    GARB_IDENT,
    GARB_FUNCTION,          // name, and its arguments as children
    GARB_AT_KEYWORD,
    GARB_HASH,              // `id` says whether it would make an ident
    GARB_STRING,
    GARB_BAD_STRING,        // a newline before the closing quote
    GARB_URL,
    GARB_BAD_URL,
    GARB_DELIM,             // one code point, in `text`
    GARB_NUMBER,
    GARB_PERCENTAGE,
    GARB_DIMENSION,         // `unit` is its unit
    GARB_WHITESPACE,
    GARB_CDO,
    GARB_CDC,
    GARB_COLON,
    GARB_SEMICOLON,
    GARB_COMMA,
    GARB_BLOCK,             // `{}`, `[]` or `()`: `open` says which; children
    // A closing bracket with nothing open to close, kept where it stood so
    // a reader sees where the text went wrong: `open` is the bracket.
    GARB_UNMATCHED,
} garb_kind_t;

typedef struct garb_value garb_value_t;

struct garb_value {
    garb_kind_t kind;
    // IDENT, FUNCTION, AT_KEYWORD, HASH, STRING, URL: the value, escapes
    // resolved. DELIM: the code point. NUMBER, PERCENTAGE, DIMENSION: the
    // number as it was written (`+12`, `1e3`), which An+B and a faithful
    // serialization both need.
    const char *text;
    size_t len;
    double number;
    bool integer;           // written without a fraction or an exponent
    bool id;                // HASH: its text would make an ident
    // STRING, URL: the text ended before the closing quote or parenthesis.
    // The token stands — the specification closes it for you — and a
    // reader that cares where the text broke off can see that it did.
    bool eof;
    const char *unit;       // DIMENSION
    size_t unit_len;
    char open;              // BLOCK: '{', '[' or '('; UNMATCHED: '}', ']' or ')'
    garb_value_t *children; // FUNCTION arguments, BLOCK contents
    int32_t nchildren;
};

// ── Rules, declarations, and the lists they come in (§5) ────────────────

typedef struct garb_rule garb_rule_t;

typedef struct {
    const char *name;
    size_t len;
    garb_value_t *value;    // white space trimmed from both ends, `!important` removed
    int32_t nvalue;
    bool important;
} garb_decl_t;

// One entry of a list the parser produces: a declaration, a rule, or the
// place where one was thrown away as invalid. The ORDER is the text's, and
// it matters: in a block, CSS Nesting ranks declarations and the rules
// nested among them by where they stand.
typedef enum { GARB_ITEM_DECL, GARB_ITEM_RULE, GARB_ITEM_INVALID } garb_item_kind_t;

typedef struct {
    garb_item_kind_t kind;
    garb_decl_t decl;       // DECL
    garb_rule_t *rule;      // RULE
} garb_item_t;

// A rule: a QUALIFIED rule (a style rule, `p { … }`) or an AT-RULE
// (`@media …`). Its block is kept as the component values the parser made
// of it, as §5 produces them: what a block holds is its rule's business —
// a style rule's declarations and the rules nested among them are read
// with garb_items_of, a `@media` block's rules with garb_rules_of — so the
// reading is done once, by whoever knows the rule.
struct garb_rule {
    bool at;
    const char *name;       // AT: the name without its '@'
    size_t len;
    garb_value_t *prelude;
    int32_t nprelude;
    bool has_block;         // an at-rule may have none (`@import …;`)
    garb_value_t *block;    // the block's contents
    int32_t nblock;
};

// ── Results ─────────────────────────────────────────────────────────────

typedef enum {
    GARB_OK = 0,
    GARB_NO_MEMORY,         // nothing came back at all
    GARB_TOO_BIG,           // the input is past GARB_SHEET_MAX
    // The single-item parses (§5.3.x): nothing there, or more than one.
    GARB_EMPTY,
    GARB_EXTRA_INPUT,
    GARB_INVALID,
} garb_status_t;

// A sheet is refused whole past this: the web's largest sheets are a
// fraction of it, and a sheet is held whole while it is parsed.
#define GARB_SHEET_MAX ((size_t)8 << 20)
// Blocks and functions nest this deep and no deeper: past it their
// contents are read and thrown away, so the end is still found, and the
// result says it is incomplete.
#define GARB_DEPTH_MAX 64


// What every parse returns: an arena that owns it all, and whether the
// parse ran short of memory or of depth partway — what came back is real,
// and there is less of it than the text held.
typedef struct {
    struct os64_arena *arena;
    bool incomplete;
    garb_item_t *items;     // a list of rules or of a block's contents
    int32_t nitems;
    garb_value_t *values;   // a list of component values
    int32_t nvalues;
    garb_rule_t *rule;      // garb_parse_one_rule
    garb_decl_t decl;       // garb_parse_one_declaration
    // garb_sheet_parse: the encoding the bytes were read in (§3.2) — a
    // BOM's, the protocol's, an @charset's, the environment's, or UTF-8.
    const char *encoding;
} garb_parsed_t;

// §3.2 and §5.3.3: a stylesheet from BYTES into `items`. `protocol` is the
// encoding the transport named (a Content-Type charset), `environment` the
// referring document's; either may be NULL.
garb_status_t garb_parse_sheet(const uint8_t *bytes, size_t len, const char *protocol,
                               const char *environment, garb_parsed_t *out);

// The rest take TEXT already in UTF-8.
// A stylesheet (§5.3.3) — a `style` element's contents — into `items`.
garb_status_t garb_parse_sheet_text(const char *text, size_t len, garb_parsed_t *out);
// A list of rules (§5.3.4) into `items`.
garb_status_t garb_parse_rules(const char *text, size_t len, garb_parsed_t *out);
// A block's contents (§5.3.6) — what a `style` attribute is — into `items`.
garb_status_t garb_parse_block(const char *text, size_t len, garb_parsed_t *out);
// A list of declarations: a block's contents without its qualified rules,
// kept for the suite's `declaration_list` and for at-rules that take one.
garb_status_t garb_parse_declaration_list(const char *text, size_t len, garb_parsed_t *out);
// A list of component values (§5.3.10) into `values`.
garb_status_t garb_parse_values(const char *text, size_t len, garb_parsed_t *out);
// One of each (§5.3.5, §5.3.7, §5.3.9): EMPTY, EXTRA_INPUT or INVALID when
// the text is not exactly one.
garb_status_t garb_parse_one_rule(const char *text, size_t len, garb_parsed_t *out);
garb_status_t garb_parse_one_declaration(const char *text, size_t len, garb_parsed_t *out);
garb_status_t garb_parse_one_value(const char *text, size_t len, garb_parsed_t *out);

// An at-rule's block, read as rules or as a block's contents, into the
// arena of the parse it came from, so the answer lives and dies with it.
bool garb_rules_of(garb_parsed_t *owner, const garb_value_t *values, int32_t n,
                   garb_item_t **items, int32_t *nitems);
bool garb_items_of(garb_parsed_t *owner, const garb_value_t *values, int32_t n,
                   garb_item_t **items, int32_t *nitems);

// Frees everything a parse returned — and a parse that answered anything
// but OK is freed too, since what it had begun is in the arena. A zeroed or
// already-freed result is fine.
void garb_free(garb_parsed_t *parsed);

// ── Dumps ───────────────────────────────────────────────────────────────
//
// JSON in css-parsing-tests' own representation, for the harness and a
// probe in the guest. Like snprintf: answers the length the whole dump
// needs and writes what fits.
size_t garb_dump_values(const garb_value_t *v, int32_t n, char *out, size_t cap);
size_t garb_dump_items(const garb_item_t *items, int32_t n, char *out, size_t cap);
size_t garb_dump_rule(const garb_rule_t *rule, char *out, size_t cap);
size_t garb_dump_decl(const garb_decl_t *decl, char *out, size_t cap);

#pragma GCC visibility pop

#endif
