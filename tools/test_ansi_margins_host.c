// Paint real renderer margins into RAM, including non-cell-aligned geometry
// and pitch padding. No privileged renderer entry points are invoked.
// Keep the unused kernel printf from interposing the host libc symbol.
#define printf kernel_printf
#include "../kernel/src/BasicRenderer.c"
#undef printf

extern void abort(void);
extern int puts(const char *s);
BasicRenderer kRenderer;

static void check(bool ok)
{
    if (!ok) { puts("FAIL framebuffer margin coverage"); abort(); }
}

int main(void)
{
    uint32_t pixels[24 * 35], shadow[24 * 35];
    struct Framebuffer fb = {.base_address = pixels, .width = 19,
                            .height = 35, .pixels_per_scan_line = 24};
    kRenderer.framebuffer = &fb;
    const uint32_t untouched = 0xabcdef;
    // The margin is whatever the CELL leaves over, so it is checked at two
    // cells: 8x16 leaves 3 columns and 3 lines of this 19x35 surface, 5x9
    // leaves 4 columns and 8 lines. Only the cell is read by the margin
    // painter; no glyph is drawn, so the face carries none.
    static const struct { unsigned w, h; } cells[] = {{8, 16}, {5, 9}};
    for (unsigned c = 0; c < 2; c++) {
        kRenderer.face = (console_face_t){.width = cells[c].w, .height = cells[c].h,
                                          .row_bytes = 1, .glyph_bytes = cells[c].h};
        unsigned cell_w = 19 / cells[c].w * cells[c].w;
        unsigned cell_h = 35 / cells[c].h * cells[c].h;
        for (unsigned mode = 0; mode < 3; mode++) {
            kRenderer.shadow = mode == 2 ? NULL : shadow;
            s_glassDirty = mode == 1;
            for (unsigned i = 0; i < 24 * 35; i++)
                pixels[i] = shadow[i] = untouched;
            renderer_glass_background_locked(0x123456);
            for (unsigned y = 0; y < 35; y++) {
                for (unsigned x = 0; x < 24; x++) {
                    bool margin = x < 19 && (x >= cell_w || y >= cell_h);
                    unsigned i = y * 24 + x;
                    check(pixels[i] == (margin && mode != 1 ? 0x123456 : untouched));
                    check(shadow[i] == (margin && mode != 2 ? 0x123456 : untouched));
                }
            }
        }
    }
    check(kFrameBufferBackgroundColor == 0x123456);
    puts("test_ansi_margins_host: all checks passed");
    return 0;
}
