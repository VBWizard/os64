#ifndef OS64_DRAW2D_BLUR_H
#define OS64_DRAW2D_BLUR_H
#include <draw2d/paint.h>

/* Scalar coverage, with no assumed normalization: CSS uses 0..256 and
 * other callers may use the full uint16_t range. Stride/capacity are bytes;
 * capacity need not include padding after the final row. */
typedef struct {
    uint16_t *pixels;
    uint32_t width, height;
    size_t stride, capacity;
} draw2d_mask16_t;

#pragma GCC visibility push(default)
/* Three separable box blurs, each with the given integer radius. Values
 * outside the mask are `edge`; each pass rounds to the nearest integer.
 * The caller supplies max(width,height) uint16_t scratch entries, disjoint
 * from the mask. No allocation or callbacks. Work is O(width*height), even
 * when radius exceeds the dimensions. max_pixels admits the mask area.
 * Invalid storage or insufficient scratch/work budget leaves pixels intact.
 * Empty masks and radius zero need no scratch; nonempty masks still require
 * valid storage. Empty masks may have NULL pixels. */
draw2d_status_t draw2d_blur_mask16(const draw2d_mask16_t *, uint32_t radius,
                                 uint16_t edge, uint16_t *scratch,
                                 size_t scratch_capacity, size_t max_pixels);
#pragma GCC visibility pop
#endif
