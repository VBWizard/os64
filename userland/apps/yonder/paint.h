#ifndef YONDER_PAINT_H
#define YONDER_PAINT_H

// The page view's painter (YONDER.md § The page view): a laid-out tree and
// a viewport in, drawing out — through its verbs, never straight to a
// surface, so the host harness can record what a page paints and check it
// by hand the way libflow's dumps are checked.

#include "flow/flow.h"
#include "scratch.h"

// Everything is in PAGE coordinates — a box in a fixed subtree moved there
// by the scroll, which the painter does for every verb — and the executor
// moves it onto the glass; between a through_open and its close, in that
// group's own coordinates instead. `clip` is the viewport, handed to every verb
// because a text run and a picture are drawn whole and must be cut by
// whoever draws them.
// A colour is flow's (flow.h): its top byte how transparent it is, which a
// verb blends by.
typedef struct {
    void *ctx;
    void (*fill)(void *ctx, os64_gui_rect_t rect, uint32_t colour);
    // A TEXT or MARKER box: its run with its origin at (x, baseline), which
    // is the box's rect.x and baseline in page coordinates.
    void (*text)(void *ctx, const flow_box_t *box, int32_t x, int32_t baseline,
                 os64_gui_rect_t clip, uint32_t colour);
    // A fetched picture or an outer inline SVG viewport, and a form control, at the content rectangle.
    void (*image)(void *ctx, const flow_box_t *box, os64_gui_rect_t content, os64_gui_rect_t clip);
    void (*control)(void *ctx, const flow_box_t *box, os64_gui_rect_t content,
                    os64_gui_rect_t clip);
    // A picture BEHIND a box (YONDER.md § Y5b). With `area` NULL, only
    // asks: true when a sheet or an attribute puts one there. Otherwise
    // sheet layer `layer`'s picture — or, at -1, the `background`
    // attribute's — is tiled across `area` (the layer's clip box or a part
    // of it, as a rounded box's is drawn, or the view for the canvas's): a
    // layer's as its size and position place it
    // in `origin` (yonder_background_tile), an attribute's from (ox, oy);
    // drawn if it has arrived, nothing yet if not. `origin` is the
    // painter's answer to which box: the layer's origin box of the box, or
    // for the canvas the ROOT's, whichever element the picture came from
    // (Backgrounds 3 § 2.11.2).
    bool (*backdrop)(void *ctx, const flow_box_t *box, int32_t layer, const os64_gui_rect_t *area,
                     os64_gui_rect_t origin, int32_t ox, int32_t oy, os64_gui_rect_t clip);
    // A GROUP below full opacity (flow_visit_groups) and not laid through
    // its matrix (through_open, below): everything painted between an open
    // and its close is laid over what was under it at `alpha` of 255 (CSS
    // Color 4 § 3.2). `bounds` holds all of it, cut to the view — empty
    // when none of it shows — in the coordinates being painted. Groups
    // nest. Either NULL: groups are painted opaque.
    void (*group_open)(void *ctx, os64_gui_rect_t bounds);
    void (*group_close)(void *ctx, uint8_t alpha);
    // A SHADOW's pixels: `colour`'s RGB laid over each pixel of `rect`
    // (page coordinates, already cut to the viewport) at its own `alpha`,
    // `rect.w` a row, alpha 0 leaving the pixel as it is.
    void (*mask)(void *ctx, os64_gui_rect_t rect, const uint8_t *alpha, uint32_t colour);
    // A picture the painter made — a GRADIENT's pixels: `rect` (page
    // coordinates, already cut to the viewport) of 0xAARRGGBB, straight
    // alpha, `rect.w` a row, each laid over what is there.
    void (*pixels)(void *ctx, os64_gui_rect_t rect, const uint32_t *argb);
    // A STAND-IN for what the face cannot draw in a box, in `ink`, across
    // `content`. A FRAME (an `iframe` or a `frame`), on `paper`: no
    // document is loaded inside a document, so the box says where its
    // document is, and a click there follows it (libpage makes the frame a
    // link). A CANVAS's replaced box (flow_box_t.replaced; a canvas laid
    // out otherwise is its fallback, drawn as content): nothing draws what
    // its script draws, so the box says so, with no paper, since a canvas
    // is transparent until drawn and what is under it must show. NULL: the
    // box is drawn empty.
    void (*stand_in)(void *ctx, const flow_box_t *box, os64_gui_rect_t content, os64_gui_rect_t clip,
                     uint32_t ink, uint32_t paper);
    // A group DRAWN THROUGH its matrix (flow_box_through; PILE3.md §
    // Transforms, the painter): everything painted until its close is in
    // the group's own coordinates, inside `bounds` there, and is then laid
    // through `m` — x' = m[0] x + m[2] y + m[4], y' = m[1] x + m[3] y +
    // m[5] — onto the coordinates painted before the open, inside `clip`
    // there, at `alpha` of 255. False when it cannot be laid (no memory,
    // too large): the painter then draws the group upright, and every verb
    // it calls is in the coordinates painted before. Either NULL: no group
    // is laid through.
    bool (*through_open)(void *ctx, os64_gui_rect_t bounds, const double m[6],
                         os64_gui_rect_t clip);
    void (*through_close)(void *ctx, uint8_t alpha);
    // A TEXT CLIP (PILE3.md § The text clip): a group whose opacity is per
    // pixel. After clip_open, what is painted is the MASK — the clip's text
    // in white, nothing else, and its coverage is the white; after
    // clip_fill, what is painted is the group; at clip_close the group is
    // laid over what was under it at each pixel's coverage. `bounds` holds
    // all of both, in the coordinates being painted. False when it cannot
    // be made (no memory, too large, one already open): the painter draws
    // the text in one colour instead, and paints nothing between. Any
    // NULL: no clip is made.
    bool (*clip_open)(void *ctx, os64_gui_rect_t bounds);
    void (*clip_fill)(void *ctx);
    void (*clip_close)(void *ctx);
    // A FILTERED group (flow_style_t.filter; FILTER.md): everything
    // painted until its close goes into a layer, `bounds` of the
    // coordinates being painted, which do not change; at the close the
    // list `filter` (n functions, none a URL) is run over it (filter.h)
    // and the result is laid over what was under it at `alpha` of 255,
    // `current` the colour of a drop shadow in currentColor. False when it
    // cannot be made (no memory, too large): the painter paints the group
    // unfiltered. Either NULL: no filter is drawn.
    bool (*filter_open)(void *ctx, os64_gui_rect_t bounds);
    void (*filter_close)(void *ctx, const flow_filter_t *filter, int32_t n, uint32_t current,
                         uint8_t alpha);
    // A BACKDROP (flow_style_t.backdrop_filter; FILTER.md § 7): what is
    // painted under `rect` (a border box cut to the view, in the
    // coordinates being painted) — read with the list's reach round it —
    // filtered by `filter` (n functions, none a URL; `current` a drop
    // shadow's currentColor) and laid back inside `rect`, at the coverage
    // `cover` gives each of its pixels (rect.w a row; NULL for all of it).
    // False, and nothing changed, when it cannot be drawn: no memory, or
    // nothing behind it that is the page (inside a group's sheets). NULL:
    // no backdrop is filtered.
    bool (*backdrop_filter)(void *ctx, os64_gui_rect_t rect, const uint8_t *cover,
                            const flow_filter_t *filter, int32_t n, uint32_t current);
} yonder_verbs_t;

// Whose background is the CANVAS's (CSS 2.1 § 14.2): the root's when it has
// one, colour or picture, else the body's when it has one, else none. The
// painter's own rule, for a face that must know it apart from painting —
// the canvas is behind the whole view wherever its owner's box has gone.
// `verbs->backdrop` is asked with no area, which only asks.
const flow_box_t *yonder_canvas_owner(const flow_tree_t *tree, const yonder_verbs_t *verbs);

// DARK PAGES (yonder.conf's `appearance = dark`): every colour the page
// paints is drawn at its brightness moved, hue and saturation kept
// (paint.c's relight). A background or a gradient is PAPER, and goes dark
// if it looks brighter than mid grey; text, a decoration, a bullet or a
// border is INK, and goes light if it looks darker, so a moved ink on a
// moved paper is always far apart. Each other colour, and every picture,
// is the page's. A page with a dark design of its own (it was told, by
// `prefers-color-scheme`) has nothing to move. `paper` is the canvas of a
// page that set none.
typedef struct {
    uint32_t paper;
} yonder_dark_t;

// THE PAINT CACHE (YONDER_PERF.md § perf-2): a gradient's tile and a
// blurred shadow's mask, kept between paints in 128-pixel blocks and found
// by the values that make them, so a paint copies its part out instead of
// working it out again. What a paint draws from it is what it draws
// without it, pixel for pixel. One window's, on its one thread: no lock.
typedef struct yonder_paint_cache yonder_paint_cache_t;
// THE BUDGET IS THE SCREEN'S (yonder_paint_cache_budget): what the cache
// keeps is the view's share of each picture, so the most it can want grows
// with the largest the view can be, which is the screen, and the screen is
// asked once, when the cache is made. Never the window's size: that changes
// at every drag, and a budget that shrank with it would evict mid-gesture
// what the next frame wants back. SCREENS of them, because a box can stack
// a view's share of several gradients and shadows (Scratch 2000's panel
// lays four), never more than a SHARE of the machine's installed memory (a
// big panel on a small machine), never less than the FLOOR, and never more
// than the CEILING (a 5K panel is about 450 MiB of it). Installed, not
// free: the budget is set once, and what was free at the first paint says
// nothing a minute later; a make the heap refuses below the budget lets
// go of older entries instead (paint_cache_make).
#define YONDER_PAINT_CACHE_FLOOR   ((size_t)32 << 20)
#define YONDER_PAINT_CACHE_CEILING ((size_t)1 << 30)
#define YONDER_PAINT_CACHE_SCREENS 8
#define YONDER_PAINT_CACHE_SHARE   16   // at most 1/16 of installed memory
// The budget for a screen of `width` x `height` pixels on a machine with
// `memory` bytes installed (0 x 0, or 0 bytes, when not known: the floor).
size_t yonder_paint_cache_budget(uint32_t width, uint32_t height, uint64_t memory);
// An empty cache holding at most `budget` bytes, or NULL for no memory.
yonder_paint_cache_t *yonder_paint_cache_new(size_t budget);
// Every entry let go (another page is shown); the counts kept. NULL: nothing.
void yonder_paint_cache_empty(yonder_paint_cache_t *c);
void yonder_paint_cache_free(yonder_paint_cache_t *c);
// Since the counts last restarted: blocks found (`hits`) and made, blocks
// let go for room, and pictures worked out without the cache because their
// blocks did not fit (what the last two paints used is not let go) or
// found no memory. And what is held now, in bytes and blocks, against the
// budget. All zero for NULL.
typedef struct {
    uint64_t hits, made, evicted, no_room, no_memory;
    uint64_t bytes, entries, budget;
} yonder_paint_cache_counts_t;
yonder_paint_cache_counts_t yonder_paint_cache_counts(const yonder_paint_cache_t *c);
void yonder_paint_cache_counts_restart(yonder_paint_cache_t *c);

// Paints every box that meets `viewport`, canvas first, the page scrolled
// to `scroll` (flow_visit's). `paper` is the canvas when neither the root
// nor the body has a background, colour or picture; `dark`, when not NULL,
// paints the page dark (above) and its paper is the canvas instead.
// `cache`, when not NULL, keeps what the painter works out between paints
// (above); NULL paints the same pixels, working everything out. `scratch`
// (scratch.h) lends the painter its working buffers; NULL takes each from
// the heap and frees it, the same pixels again.
// The painter's own work a caller may time: building and blurring a
// shadow's mask, and computing a gradient's pixels, or copying either out
// of the cache — the verbs it calls are the caller's to time. When set
// (yonder's paint counts, YONDER_PERF.md) it is told each one's start
// (`begin`) and end; NULL, nothing is told.
typedef enum { YONDER_PAINT_SHADOW, YONDER_PAINT_GRADIENT } yonder_paint_stage_t;
extern void (*yonder_paint_stage)(yonder_paint_stage_t stage, bool begin);
// How many line cuts (flow_line_cut) the painter has worked, since the
// program started: a count the harness holds to the lines painted, not the
// pieces on them.
extern uint64_t yonder_paint_cuts;
// How many filtered groups and backdrops the last yonder_paint drew
// unfiltered: past its
// budget for filter layers — YONDER_FILTER_AREA_VIEWS times the
// viewport's pixels, each layer charged the pixels it shows in the
// viewport, FILTER.md § 6 — or a layer the verbs refused or found no
// memory to filter. The caller's to name in the record; the verbs count
// what they refuse at the close themselves.
extern uint32_t yonder_paint_filters_refused;
#define YONDER_FILTER_AREA_VIEWS 2

void yonder_paint(const flow_tree_t *tree, os64_gui_rect_t viewport, flow_point_t scroll,
                  uint32_t paper, const yonder_dark_t *dark, yonder_paint_cache_t *cache,
                  yonder_scratch_t *scratch, const yonder_verbs_t *verbs);

// One of `box`'s boxes — its border box `rect` (in any coordinates), or
// the padding or content box inside it. A percentage padding counts as
// none, as content_of's does; TEXT is the border box.
os64_gui_rect_t yonder_box_edge(const flow_box_t *box, os64_gui_rect_t rect, flow_edge_t edge);

// A sheet's background layer, one copy of it: where it is and how big, and
// on which axes it repeats (CSS Backgrounds 3 § 3.4–§ 3.9), from layer `l`
// and its ORIGIN box `area`. `iw` x `ih` is the picture's own size, 0 x 0
// for one with none (a gradient), which is then the origin box's. A
// percentage position is of the room the copy leaves, so 100% puts it
// against the far edge and 50% in the middle. False when the copy is
// empty, and nothing is drawn.
typedef struct {
    os64_gui_rect_t at;
    bool repeat_x, repeat_y;
} yonder_tile_t;
bool yonder_background_tile(const flow_layer_t *l, os64_gui_rect_t area, uint32_t iw, uint32_t ih,
                            yonder_tile_t *out);

// Where a picture `iw` x `ih` is drawn in its CONTENT box `box`, as its
// style's object-fit and object-position say (CSS Images 3 § 4.5, § 4.6):
// the box itself for fill, or a picture without a size on both axes; its
// own ratio scaled to fit inside the box (contain) or to cover it (cover);
// its own size (none); none if that fits inside the box, else contain
// (scale-down) — placed by the position, a percentage of the room it
// leaves, which is negative where it is bigger than the box. What is
// drawn of it is cut to the box: that is the caller's.
os64_gui_rect_t yonder_object_rect(const flow_style_t *s, os64_gui_rect_t box, uint32_t iw,
                                   uint32_t ih);

// The two tones a bevelled border is drawn in, from its colour.
uint32_t yonder_lighter(uint32_t colour);
uint32_t yonder_darker(uint32_t colour);

#endif
