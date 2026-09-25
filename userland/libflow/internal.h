#ifndef FLOW_INTERNAL_H
#define FLOW_INTERNAL_H

#include "flow/flow.h"
#include "os64/mem.h"
#include "os64/str.h"

#define F_ARRAY(a) ((int32_t)(sizeof(a) / sizeof((a)[0])))

// ── Storage ─────────────────────────────────────────────────────────────
//
// Everything a layout builds lives until the layout is freed and is never
// freed piecemeal, so it comes from blocks that are freed together. A
// pointer handed out stays valid: blocks never move.
typedef struct FBlock FBlock;
typedef struct {
    FBlock *blocks;
} FArena;

void *f_arena_alloc(FArena *arena, size_t size);    // zeroed; NULL on no memory
void f_arena_free(FArena *arena);

// A node to the record built for it. Open addressing over a power-of-two
// table, because layout asks "what is this node's style?" once per node
// and the face asks "which box is this node?" once per click.
typedef struct {
    const void **keys;
    void **vals;
    size_t cap, count;
} FMap;

bool f_map_put(FMap *map, const void *key, void *val);  // false on no memory
void *f_map_get(const FMap *map, const void *key);      // NULL when absent
void f_map_free(FMap *map);

// ── The page's attribute grammars (attrs.c) ─────────────────────────────
//
// HTML's microsyntaxes, each the standard's algorithm by name, so a value a
// page wrote means here what it means in every browser.

typedef enum { F_DIM_NONE = 0, F_DIM_PX, F_DIM_PERCENT } f_dim_kind_t;

// "Rules for parsing dimension values": digits, an optional fraction, and a
// `%` that makes it a percentage; anything after that is ignored. The value
// is in 1/64ths — of a pixel or of a percent. `nonzero` is the "(ignoring
// zero)" form: a zero is no value at all.
f_dim_kind_t f_parse_dimension(const char *s, bool nonzero, int32_t *out);
// "Rules for parsing integers" and "…non-negative integers". A value too
// large to mean anything on a page is clamped to F_INT_MAX, which no
// geometry reaches, rather than overflowing into a different number.
#define F_INT_MAX 1000000
bool f_parse_integer(const char *s, int32_t *out);
bool f_parse_nonnegative(const char *s, int32_t *out);
// "Rules for parsing a legacy colour value", `chucknorris` and all: a
// page's colour attribute means the colour every browser draws.
bool f_parse_legacy_color(const char *s, uint32_t *out);

// ASCII case-insensitive, which is what `[attr=value i]` means.
bool f_eq_nocase(const char *a, const char *b);

// ── Pass 1: the computed styles (style.c) ───────────────────────────────

typedef struct {
    flow_style_t style;
    // Pass 2's question, answered bottom-up here: does this element make,
    // somewhere inside its inline formatting, an in-flow block-level box?
    // True for an inline with a block child (or an inline child that holds
    // one), which is what makes its container MIX and the inline SPLIT
    // (LAYOUT.md § Pass 2). Not a style property: no cascade writes it.
    bool holds_block;
} FStyled;

typedef struct {
    const os64_html_document_t *doc;
    const flow_env_t *env;
    FArena arena;
    FMap map;               // element node -> FStyled
} FStyles;

// ALL OR NOTHING: NULL when memory runs out anywhere. A style is a few
// hundred bytes per element, so a page that cannot have its styles cannot
// have its boxes either; and a partial table would let pass 2 decide a
// container's shape from half its children, which a whole build decides
// differently (LAYOUT.md § Proof, the prefix rule).
FStyles *f_style_build(const os64_html_document_t *doc, const os64_page_t *model,
                       const flow_env_t *env);
// NULL for a node with no style: not an element, or inside a
// `display: none` subtree, which pass 1 does not descend into.
const FStyled *f_style_of(const FStyles *styles, const os64_html_node_t *node);
void f_style_free(FStyles *styles);

// One line per styled element, indented by depth: the element, its display,
// then what it CHANGED — an inherited property where it differs from the
// parent's, any other where it differs from its initial value — so a line
// reads as the rules that fired. Like snprintf, it answers the length the
// whole dump needs and writes what fits.
int64_t f_style_dump(const FStyles *styles, char *out, size_t cap);

#endif
