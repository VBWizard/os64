#ifndef GUI_VISIBLE_H
#define GUI_VISIBLE_H

// Which part of a rectangle each window can be seen in (OCCLUSION.md):
// given a rectangle D and the frames of the windows over it, top-down, the
// pieces of D each frame claims, and what no frame claims. No kernel
// headers, so tools/test_visible_host.sh runs this file under the
// sanitizers against a pixel oracle.

#include <stdint.h>

typedef struct {
    int32_t x, y, w, h;
} visible_rect_t;

// A piece of D: `owner` is the index (into the frames given) of the frame
// that claims it, or VISIBLE_UNCLAIMED.
typedef struct {
    visible_rect_t r;
    int32_t owner;
} visible_piece_t;

#define VISIBLE_UNCLAIMED (-1)

// The most pieces one call may hold, claimed and unclaimed together.
#define VISIBLE_PIECES_MAX 64

// Walks `frames` (`nframes` of them, frames[0] topmost) over D. Each frame
// claims the parts of what is still unclaimed that it covers. Writes the
// pieces to `out` (room for `cap`, at least 1; no more than
// VISIBLE_PIECES_MAX are written whatever `cap` says) and answers how
// many; the pieces partition D. D's edges must fit in an int32_t (it is a
// part of the screen); a frame may lie anywhere. `*consumed` is how many frames were
// applied: all of them, unless applying the next would have needed more
// than `cap` pieces, and then the walk stopped before it. The unclaimed
// pieces are then what the frames from *consumed on, and whatever lies
// under them, must still paint; none of the consumed frames touches one.
int32_t visible_claim(visible_rect_t d, const visible_rect_t *frames, int32_t nframes,
                      visible_piece_t *out, int32_t cap, int32_t *consumed);

#endif
