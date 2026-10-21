#ifndef YONDER_BACKDROP_H
#define YONDER_BACKDROP_H

// A moving background drawn by its own pixels (BACKDROP_LAYER.md). The view
// is painted three times with the moving layer of one box replaced: black
// (K), white (W), and left out (P). Everything the painter draws in front of
// a background is `over`, which is affine in what is under it, so a pixel
// whose moving layer shows `b` at alpha `a` is
//
//     F = K + (W - K) b / 255          the front over b, as if b were opaque
//     out = P + (F - P) a / 255        b laid over what is behind it
//
// per channel — warp.h's two sheets, with a third for what shows through.
// A frame of the moving layer then costs its changed pixels, not a paint of
// everything in front of it. Pure over pixels and numbers, so the host
// harness checks it against `over` itself.

#include <stdbool.h>
#include <stdint.h>

#include "os64/rect.h"

// The three paintings, `w` x `h` (the view's size), 0x00RRGGBB, `pitch`
// pixels a row. Nothing is composed off them, whatever a caller's
// rectangles say: they are the view, and `dst` is as big.
typedef struct {
    const uint32_t *k, *w, *p;
    uint32_t pitch;
    int32_t width, height;
} yonder_backdrop_sheets_t;

// The moving layer as laid: `w` x `h` pixels of 0xAARRGGBB with its pixel
// (0, 0) at (x, y) in view coordinates, copied on in every direction along
// an axis that repeats. Off its one copy, along an axis that does not, the
// layer is absent: the pixel is P.
typedef struct {
    const uint32_t *px;
    int32_t w, h;
    int32_t x, y;
    bool repeat_x, repeat_y;
} yonder_backdrop_tile_t;

// One pixel: the three paintings' and the moving layer's.
uint32_t yonder_backdrop_pixel(uint32_t k, uint32_t w, uint32_t p, uint32_t b);

// A row's run of changed tile pixels: `n` from (x, y), tile coordinates.
typedef struct {
    int32_t y, x, n;
} yonder_backdrop_run_t;

// The pixels where two tiles of `w` x `h` differ, as runs, into `runs` (at
// most `cap`), and how many pixels in `*changed`. The number of runs, or -1
// when they did not fit: the caller composes the whole layer instead.
int32_t yonder_backdrop_changes(const uint32_t *was, const uint32_t *now, int32_t w, int32_t h,
                                yonder_backdrop_run_t *runs, int32_t cap, int64_t *changed);

// What a compose skips and what it reports. `skip` are view rectangles left
// alone (the form widgets' holes, what was just composed whole); each
// rectangle that received pixels is added to `damage` by the dirty list's
// rule (os64_damage_add), so `damage` covers every pixel written; `written`
// counts them.
typedef struct {
    const os64_gui_rect_t *skip;
    uint32_t nskip;
    os64_gui_rect_t *damage;
    uint32_t ndamage, cap;
    int64_t slack;
    int64_t written;
} yonder_backdrop_out_t;

// Every pixel of `area` inside `part` (the moving box's part of the view),
// outside `out->skip`, composed into `dst` (the view's pixel (0, 0), `pitch`
// pixels a row).
void yonder_backdrop_compose_area(uint32_t *dst, uint32_t pitch, const yonder_backdrop_sheets_t *s,
                                  const yonder_backdrop_tile_t *t, os64_gui_rect_t part,
                                  os64_gui_rect_t area, yonder_backdrop_out_t *out);

// The changed pixels `runs` (yonder_backdrop_changes of the shown tile and
// `t`'s) at every copy of the tile that meets `part`, outside `out->skip`,
// composed into `dst` as yonder_backdrop_compose_area would compose them.
void yonder_backdrop_compose_runs(uint32_t *dst, uint32_t pitch, const yonder_backdrop_sheets_t *s,
                                  const yonder_backdrop_tile_t *t, os64_gui_rect_t part,
                                  const yonder_backdrop_run_t *runs, int32_t nruns,
                                  yonder_backdrop_out_t *out);

// ── When the layer is worth having ──────────────────────────────────────
//
// The layer must never make a page slower than painting it ordinarily
// (BACKDROP_LAYER.md § What "nothing slower" means). Its first build costs
// three paints where an ordinary frame costs one, so it is built only once
// the view has been quiet, and a build thrown away before it paid for
// itself doubles the quiet the next one needs. Each frame, a built layer
// composes only when that is measured cheaper than painting, and a layer
// that stays dearer while valid is given up for good.

// Frames of quiet a build needs at first, and the most a back-off asks.
#define YONDER_BACKDROP_QUIET 2u
#define YONDER_BACKDROP_QUIET_MOST 64u
// Frames running on a valid layer that measured dearer than painting
// before the layer is given up.
#define YONDER_BACKDROP_DEARER 8u

// Times are in whatever unit the caller measures with, one unit throughout
// (yonder: the cycle counter's).
typedef struct {
    // A pixel's time, measured; 0 before the first measurement.
    double paint_cost;      // an ordinary paint of the moving box's part
    double compose_cost;    // a composed pixel
    double pass_cost;       // one of the three paintings
    uint32_t quiet;         // frames running with the view quiet
    uint32_t need;          // the quiet a build needs
    uint32_t dearer;        // frames running, on a valid layer, dearer than painting
    bool unprofitable;      // given up: never built again for this box
    double build_cost;      // what the standing build took
    double saved_cost;      // what its composed frames saved since
    bool paid;              // saved_cost has reached build_cost
} yonder_backdrop_policy_t;

void yonder_backdrop_policy_init(yonder_backdrop_policy_t *p);
// A frame of the moving layer came after the view changed since the last
// (the caller says how much change still counts as quiet): the quiet
// counts from nothing again.
void yonder_backdrop_ordinary(yonder_backdrop_policy_t *p);
// A frame of the moving layer came with the view quiet since the last.
void yonder_backdrop_quiet_frame(yonder_backdrop_policy_t *p);
// A frame painted ordinarily: `spent` for the `px` pixels of the box's part.
void yonder_backdrop_painted(yonder_backdrop_policy_t *p, double spent, int64_t px);
// Whether to build the layer at this frame.
bool yonder_backdrop_should_build(const yonder_backdrop_policy_t *p);
// The layer was built: the three paintings took `spent` over `px` pixels
// each.
void yonder_backdrop_built(yonder_backdrop_policy_t *p, double spent, int64_t px);
// Whether a built layer composes this frame: `changed` pixels to compose,
// `invalid` to paint three times first, against `part` painted once. False
// on a valid layer (`invalid` 0) counts toward giving it up.
bool yonder_backdrop_should_compose(yonder_backdrop_policy_t *p, int64_t changed, int64_t invalid,
                                    int64_t part);
// A frame composed: `spent` for the `written` pixels, after `rebuild_cost`
// for the three paintings of `rebuilt` invalid pixels, where an ordinary
// paint of `part` would have been.
void yonder_backdrop_composed(yonder_backdrop_policy_t *p, double spent, int64_t written,
                              double rebuild_cost, int64_t rebuilt, int64_t part);
// The built layer went wholly invalid: a waste when it had not paid.
void yonder_backdrop_lost(yonder_backdrop_policy_t *p);

#endif
