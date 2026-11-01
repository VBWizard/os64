#ifndef GARB_CASCADE_H
#define GARB_CASCADE_H

// The cascade (GARB.md, G2c): the page's sheets and its `style`
// attributes, judged against the tree and the viewport, into each
// element's AUTHOR-origin winners — the declared value of every property
// some author rule set, `var()` and `env()` already replaced. libflow
// applies them after the Rendering chapter and the presentational hints,
// and computes them.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "garb/garb.h"
#include "garb/select.h"
#include "garb/values.h"
#include "html/html.h"

#pragma GCC visibility push(default)

// What a media query and a viewport unit are judged against, and which
// properties the cascade reads.
typedef struct {
    double width, height;           // the viewport, in CSS pixels
    // The person's colour scheme: what `prefers-color-scheme` answers,
    // `dark` when true and `light` when false.
    bool dark;
    // Whether the person asked for less motion: what
    // `prefers-reduced-motion` answers, `reduce` when true and
    // `no-preference` when false.
    bool reduced_motion;
    // Whether the document's scripts run: what `scripting` answers,
    // `enabled` when true and `none` when false.
    bool scripting;
    // The output device, the screen, in CSS pixels: what device-width,
    // device-height and device-aspect-ratio answer. Either 0 answers them
    // from the viewport, for a host that knows no screen.
    double device_width, device_height;
    // The device pixels a CSS pixel is drawn with (the page's zoom): what
    // `resolution` and the WebKit pixel ratio answer, in dppx. 0 is 1.
    double resolution;
    // Whether the Modern Web arc's properties are taken away
    // (garb_reading_t): the cascade reads every declaration, and every
    // `@supports` condition, with this answer, and records a marked
    // property it then cannot read as it records any other
    // (garb_cascade_skips). Clear, the default an environment built field
    // by field gets, knows them.
    bool modern_off;
} garb_env_t;

// An `@import` (Cascade 4 § 2.1): the address as written — resolved
// against the IMPORTING sheet's address by whoever fetches it — and the
// conditions on it, as component values pointing into the importing
// sheet's parse.
typedef struct {
    const char *url;
    size_t len;
    const garb_value_t *media;      // a media query list; none holds always
    int32_t nmedia;
    bool has_supports;              // `supports(…)`: its argument
    const garb_value_t *supports;
    int32_t nsupports;
} garb_import_t;

// The @imports of `sheet` that count, in order: those written before any
// rule but @charset and a @layer statement, each with an address. Answers
// how many there are and writes at most `cap` of them.
int32_t garb_sheet_imports(const garb_parsed_t *sheet, garb_import_t *out, int32_t cap);

// One sheet, in cascade order: a parse the caller owns (it must outlive
// the cascade, which never writes to it), and the `media` its element
// gave, or NULL. An imported sheet comes BEFORE the sheet importing it —
// its rules are the importer's first — and says so: `via` is its @import,
// `parent` the importer's index in the same array. It applies when its
// @import's conditions hold and the importer applies. `via` is NULL for a
// sheet the page names itself, and is read only during garb_cascade.
typedef struct {
    const garb_parsed_t *sheet;
    const char *media;
    const garb_import_t *via;
    int32_t parent;
} garb_sheet_in_t;

// A `style` element's sheet: its child text content (HTML § 4.2.6), which
// libhtml may keep as more than one text node, parsed into `out`. The parse
// keeps no pointer into the tree: one text node is parsed where it lies —
// every string a parse keeps is its arena's copy, never its input's — and
// several are joined in a buffer freed once the parse returns.
garb_status_t garb_parse_style_element(const os64_html_node_t *style, garb_parsed_t *out);

typedef struct garb_cascade garb_cascade_t;

// An element's author-origin winners: one per property, in no particular
// order. A CSS-wide keyword (`inherit`, `initial`, `unset`, `revert`) is
// a winner like any other; libflow gives it its meaning.
typedef struct {
    const garb_set_t *sets;
    // For each winner, the index in garb_cascade's `sheets` of the sheet
    // it was written in, or -1 for the element's `style` attribute: what a
    // relative url() in it is resolved against (CSS Values 4 § 4.5.1).
    const int32_t *sheet;
    // For each winner, whether it was `!important`: an animated value
    // overrides a normal declaration and never an important one (Cascade
    // 5's order of origins).
    const bool *important;
    int32_t n;
} garb_style_t;

// Cascades `sheets` over `doc`, pinning document strings until free. NULL
// on no memory or pin exhaustion; a partial cascade says so in
// garb_cascade_incomplete. The input sheets outlive the cascade, which
// only reads them: what it makes of their rules' blocks, selectors and
// values is its own, in arenas freed with it (GARB.md § The cost), so a
// sheet is the same after any number of cascades as before the first.
garb_cascade_t *garb_cascade(const garb_sheet_in_t *sheets, int32_t n,
                             const os64_html_document_t *doc, garb_env_t env);
// Independent arena caps for an owner with a smaller working allowance.
// attribute_bytes bounds each retained style-attribute parse and the temporary
// presentation-value parse. attributes bounds their retained count. Scratch,
// slot tables and selector/match arrays are separate; these are not a unified
// allocator budget. Zero limits are literal. Input sheets remain caller-owned.
typedef struct {
    size_t state_bytes, read_bytes, attribute_bytes, attributes;
} garb_cascade_limits_t;
garb_cascade_t *garb_cascade_limited(const garb_sheet_in_t *, int32_t,
                                     const os64_html_document_t *, garb_env_t,
                                     garb_cascade_limits_t);
// The default cap on what a cascade reads of its sheets together: their rules'
// blocks, selectors and values go into one arena held to this, beside the
// cascade's own state (held to GARB_ARENA_MAX), and a cascade that reaches
// it comes up short as any short read does (garb_cascade_incomplete).
// Twice a parse's arena, because reading a sheet costs up to about twice
// its parse (GARB.md § The cost): so one cascade reads whole any one sheet
// a parse holds, but the very densest, and beside a big one the rest of an
// ordinary page — a news page's 1 MB sheet reads into 14.3 MiB, a
// Bootstrap-shaped sheet about 21.5 bytes a byte — while a page naming
// many big sheets holds no more than this of reads in any one cascade.
#define GARB_CASCADE_READ_MAX ((size_t)2 * GARB_ARENA_MAX)
bool garb_cascade_incomplete(const garb_cascade_t *c);
// Sheets are immutable while a cascade uses them. Match their identity,
// order, media/import conditions and the complete environment before updating.
// The sheet objects themselves (not just their arenas) must stay alive.
bool garb_cascade_matches(const garb_cascade_t *cascade, const garb_sheet_in_t *sheets,
                          int32_t count, garb_env_t env);
// One reference per owner; free releases it. Updates are shared and callers
// serialize them with reads. Retaining does not retain the input sheets.
bool garb_cascade_retain(garb_cascade_t *cascade);
/* Consume this cascade's native change reader and update affected winners.
 * Sheets and environment must be unchanged. False requests a fresh cascade:
 * lost history, broad dependencies/dirty sets, arena renewal or refusal.
 * On failure, previously returned values remain alive but no new queries may
 * use this cascade until it is replaced. On success, old values also remain
 * alive until free, for displayed animation records borrowing keyframes.
 * Reader storage is optional: its absence disables updates, not full builds.
 * The caller serializes updates with queries on its window thread. */
bool garb_cascade_update(garb_cascade_t *c);
// Whether this cascade keeps a change reader for incremental updates. False
// lets hosts skip speculative geometry snapshots whose generation cannot
// survive a DOM change. True is not a promise that an update will succeed:
// history loss, dirty breadth and storage budgets still require fallback.
bool garb_cascade_tracks_changes(const garb_cascade_t *c);
uint64_t garb_cascade_generation(const garb_cascade_t *c);
uint64_t garb_cascade_updates(const garb_cascade_t *c);
uint64_t garb_cascade_version(const garb_cascade_t *c);
// Whether this element's winners were recomputed after an earlier update.
bool garb_style_updated_since(const garb_cascade_t *c, const os64_html_node_t *node,
                              uint64_t update);

// The viewport the cascade was judged against: what a `vw` or `vh` in its
// winners is a hundredth of.
garb_env_t garb_cascade_env(const garb_cascade_t *c);
// `element`'s winners; an element no author rule reached has none.
garb_style_t garb_style_for(const garb_cascade_t *c, const os64_html_node_t *element);
// The winners of `element`'s ::before or ::after (GENERATED_CONTENT.md §
// 3), from that pseudo-element's rules alone: none when no rule for it
// matched, and for any other pseudo-element.
garb_style_t garb_style_for_pseudo(const garb_cascade_t *c, const os64_html_node_t *element, garb_pseudo_t pseudo);
/* True when a matched non-custom declaration needs var()/env() substitution,
 * including declarations superseded later in the cascade. Such source styles
 * cannot be reused under different inheritance without substituting again. */
bool garb_style_needs_substitution(const garb_cascade_t *c, const os64_html_node_t *element);

// One keyframe of an animation (Animations 2 § 3.2): its offset in [0, 1];
// its easing — the keyframe's own `animation-timing-function`, or NULL for
// the element's; the longhands it sets, read as winners are read, and for
// each where it was written in the rule (`order`, later greater); and, as
// a winner's, the index in garb_cascade's `sheets` of the sheet its rule
// was written in, which a relative url() in it is resolved against. Of
// several declarations of one longhand in a keyframe, the last is the one
// kept; `!important` ones and the `animation-*` longhands are not kept.
typedef struct {
    double offset;
    const garb_val_t *easing;
    const garb_set_t *sets;
    const uint64_t *order;
    int32_t nsets;
    int32_t sheet;
} garb_keyframe_t;

// The keyframes `name` names (`len` bytes, compared codepoint for
// codepoint) for `element`, in their used order — by offset, and blocks at
// one offset with one easing grouped, in the order each group was first
// written; the implicit 0% and 100% keyframes are the caller's to add, and
// so is the grouping of a block that gives the element's own easing with
// one that gives none, which only the caller can see are one (`order`).
// They come from the last `@keyframes` (or `@-webkit-keyframes`, Compat
// § 3.1) of that name in cascade order inside the `@media` and `@supports`
// that hold. A keyframe's var() is replaced against the element's custom
// properties when the element animates by that name — its winners name
// it, or it inherits names that do (`animation-name: inherit`) — and the
// declaration dropped otherwise. Answers how many and points `out` at
// them, which live as long as the cascade; -1 when no rule has that name,
// and the animation starts nothing.
int32_t garb_keyframes(const garb_cascade_t *c, const os64_html_node_t *element,
                       const char *name, size_t len, const garb_keyframe_t **out);
void garb_cascade_free(garb_cascade_t *c);

// One `@font-face` rule (Fonts 4 § 4), read whole: the family it names; its
// sources in order — a url() as written, resolved by whoever fetches it
// against the sheet the rule was written in (`sheet`, an index into
// garb_cascade's sheets), with its format() hint (one format keyword, or
// the first of a list of strings, as written; NULL with no format()) — or
// a local() family name, any length; the weights it covers, 1
// to 1000 as written, fractions and all (one weight is a range of one);
// its style; and the code points
// it covers, as inclusive ranges, NULL and 0 for all of them. A source
// with tech() is dropped (this engine supports no font technology), as is
// one written wrong; a rule left with no family or no source is no face.
typedef enum { GARB_FACE_URL, GARB_FACE_LOCAL } garb_face_src_kind_t;
typedef struct {
    garb_face_src_kind_t kind;
    const char *text;           // URL: the address; LOCAL: the family name
    size_t len;
    const char *format;         // URL: format()'s first, or NULL
    size_t format_len;
} garb_face_src_t;
typedef enum { GARB_FACE_NORMAL, GARB_FACE_ITALIC, GARB_FACE_OBLIQUE } garb_face_style_t;
typedef struct {
    uint32_t first, last;
} garb_unicode_range_t;
typedef struct {
    const char *family;
    size_t family_len;
    const garb_face_src_t *src;
    int32_t nsrc;
    float weight_min, weight_max;
    garb_face_style_t style;
    const garb_unicode_range_t *ranges;
    int32_t nranges;
    int32_t sheet;
} garb_font_face_t;

// The page's faces, from the `@font-face` rules inside the `@media` and
// `@supports` that hold, in cascade order — every one, faces of one family
// with the same weights, style and ranges among them: they make one
// composite face, the later tried first (Fonts 4 § 4.5.1), so an earlier
// one still serves when a later fails. At most GARB_FONT_FACES_MAX are
// kept; every `@font-face` rule after them is counted in `lost` (may be
// NULL) unread, face or not. So is a face memory ran out for, which is
// dropped whole — and which the cascade also says
// (garb_cascade_incomplete). Answers how many and points `out` at them,
// which live as long as the cascade.
#define GARB_FONT_FACES_MAX 256
int32_t garb_cascade_font_faces(const garb_cascade_t *c, const garb_font_face_t **out, uint32_t *lost);

// What the build passed over because this library does not read it — a
// property it has no reader for, an at-rule it does not keep, a function
// no reader accepts in a declaration it dropped, and a VALUE: `<property>: <words>` for
// a declaration of a property it reads, dropped though its value is bare
// words, or read and laid out as something else (an approximation) — each
// name once, lower case, with how often it was met. A prefixed name
// (`-webkit-…`) is not counted: one engine's dialect is passed over by
// every other. A value merely written wrong, one holding a number or a
// string, is not either. The names live as long as the cascade. Copies up to
// `cap` into `out` and answers how many there are; `lost` (may be NULL)
// is how many met no room. At most GARB_SKIPS_MAX are kept.
typedef enum { GARB_SKIP_PROPERTY, GARB_SKIP_AT_RULE, GARB_SKIP_FUNCTION, GARB_SKIP_VALUE } garb_skip_kind_t;
typedef struct {
    garb_skip_kind_t kind;
    const char *name;
    size_t len;
    uint32_t count;
} garb_skip_t;
#define GARB_SKIPS_MAX 256
int32_t garb_cascade_skips(const garb_cascade_t *c, garb_skip_t *out, int32_t cap, uint32_t *lost);

// A media query list (Media Queries 4) against `env`: a `media` attribute,
// or an @media rule's prelude. An empty list is true; one that will not
// parse is false (`not all`).
bool garb_media_matches(const garb_value_t *v, int32_t n, garb_env_t env);
bool garb_media_text_matches(const char *text, garb_env_t env);
// The same list as CSSOM serialises it, which is what a MediaQueryList's
// `media` reads (CSSOM § Serializing media queries): the queries joined by
// `, `; each one that is not a query's shape, an empty one included, as
// `not all`; `not` or `only`, the type and a known feature's name and
// keywords in lower case; `all and` left out of a query nothing negates; a
// number to six significant digits (garb_css_number), a ratio as `a / b`;
// anything else in brackets kept as written, its white space made single
// spaces. A list with no content is "". A query's shape is the grammar's:
// a feature this machine does not know changes what the query evaluates
// to, not how it is written. Each query is read once, so the cost is
// linear in the list however deep its brackets nest. Like snprintf; the
// text form answers (size_t)-1, with "" written, when it could not read the
// text whole: no memory, or brackets nested past GARB_DEPTH_MAX.
size_t garb_media_serialize(const garb_value_t *v, int32_t n, char *out, size_t cap);
size_t garb_media_text_serialize(const char *text, char *out, size_t cap);
// Memory for a buffer libgarb grows for its caller: a block of `new_size`
// bytes holding the first `old_size` of `old` (which it lets go), or NULL
// with `old` untouched; `new_size` 0 lets `old` go and answers NULL.
typedef void *(*garb_grow_t)(void *ctx, void *old, size_t old_size, size_t new_size);
// The text form in one pass into a buffer it grows with `grow` (NULL:
// libos64's os64_realloc and os64_free): NUL-terminated, its length in
// *length, the caller's to let go the way `grow` would. NULL where the
// text form answers (size_t)-1, and when there was no memory for the
// buffer, with nothing left held.
char *garb_media_text_serialized(const char *text, size_t *length, garb_grow_t grow, void *ctx);

// Every element's winners as text, in document order: `tag#id: prop:
// value; ...`, its ::before's and ::after's after it (`tag#id::before:
// ...`), for the harness and a probe. Like snprintf.
size_t garb_cascade_dump(const garb_cascade_t *c, const os64_html_document_t *doc, char *out,
                         size_t cap);

#pragma GCC visibility pop

#endif
