#ifndef YONDER_PLANE_PLACE_H
#define YONDER_PLANE_PLACE_H

// plane_place.h — whether a <video> whose player has a plane is placed on
// it at a paint, and what the page paints over its picture
// (docs/design/pending/VIDEO_PLANE.md § 5). Pure: no GUI, no player;
// tools/test_plane_place_host.sh drives it beside a model.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int32_t x, y, w, h;
} yonder_plane_rect_t;

// Why a video with a plane on is not placed, in the order they are asked
// (the first that holds is the one counted). YONDER_PLANE_PLACED is none.
typedef enum {
    YONDER_PLANE_PLACED = 0,
    YONDER_PLANE_OUT_OF_VIEW,   // no box on the shown page, unpainted, hidden, or scrolled away
    YONDER_PLANE_NOT_COVER,     // the paint does not copy its picture plain (a group, a cut line, covers off)
    YONDER_PLANE_BACKDROP,      // a box that reads what is beneath it may read the picture
    YONDER_PLANE_CLIP_MEETS,    // its clip meets the clip of a video placed before it at this paint
    YONDER_PLANE_OVER,          // the page paints over its picture, or has not yet painted all of it
    YONDER_PLANE_NOT_READY,     // the plane holds no picture of the fitted size, or not `shown`'s
    YONDER_PLANE_REFUSED,       // the kernel refused a publish placing it, a moment ago
    YONDER_PLANE_REASONS
} yonder_plane_reason_t;

typedef struct {
    bool on_view, cover, backdrop, clip_meets;
    bool over;                  // yonder_plane_map_inked, or (at the paint that places) not yet known
    bool ready;                 // the plane holds a picture of the fitted size
    bool shown;                 // that picture is the one `shown` gives
    bool held_off;              // a refused publish's hold-off runs
} yonder_plane_input_t;

typedef struct {
    yonder_plane_reason_t reason;
    bool want;                  // the player keeps the hidden plane's picture current
} yonder_plane_verdict_t;

// The verdict for one video at one paint. Wanted from "not a cover" on: a
// reason that goes away finds the plane's picture current. Out of view it
// is not, and the show thread lets pictures go by the clock.
yonder_plane_verdict_t yonder_plane_decide(const yonder_plane_input_t *in);

// The record's word for a reason.
const char *yonder_plane_reason_name(yonder_plane_reason_t r);

// WHAT THE PAGE PAINTS OVER A PICTURE, kept across partial paints: tiles of
// YONDER_PLANE_TILE pixels laid on the fitted picture's rectangle (window
// coordinates), each unknown until a paint has painted all of its part
// inside the clip, then clear or inked as that paint found it. A paint of
// only some of a tile's part can ink it, never clear it, so ink that left a
// partly painted tile stays until a paint covers the whole part: the map
// says too much, never too little.
#define YONDER_PLANE_TILE 16

typedef enum {
    YONDER_PLANE_TILE_UNKNOWN = 0,
    YONDER_PLANE_TILE_CLEAR,
    YONDER_PLANE_TILE_INKED,
} yonder_plane_tile_t;

typedef struct {
    yonder_plane_rect_t origin;     // the fitted picture the tiles are laid on; empty: no map
    uint32_t cols, rows;
    uint8_t *tiles;                 // the caller's, cols * rows (yonder_plane_tile_t)
} yonder_plane_map_t;

// The tiles needed for `origin`.
size_t yonder_plane_map_tiles(yonder_plane_rect_t origin);
// Every tile unknown, laid on `origin`, in `tiles` (`cap` of them). False,
// and no map, when `origin` is empty or needs more than `cap`.
bool yonder_plane_map_reset(yonder_plane_map_t *m, yonder_plane_rect_t origin, uint8_t *tiles, size_t cap);

// A paint of `painted` (window coordinates): for each tile meeting
// `painted` inside `clip`, `ink(ctx, part)` says whether the page painted
// anything over the picture in that part (the tile, the clip and `painted`
// met).
void yonder_plane_map_paint(yonder_plane_map_t *m, yonder_plane_rect_t painted, yonder_plane_rect_t clip,
                            bool (*ink)(void *ctx, yonder_plane_rect_t part), void *ctx);

// A tile meeting `clip` is inked.
bool yonder_plane_map_inked(const yonder_plane_map_t *m, yonder_plane_rect_t clip);
// Every tile meeting `clip` is known (clear or inked). False with no map.
bool yonder_plane_map_known(const yonder_plane_map_t *m, yonder_plane_rect_t clip);

#endif
