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
    for (unsigned mode = 0; mode < 3; mode++) {
        kRenderer.shadow = mode == 2 ? NULL : shadow;
        s_glassDirty = mode == 1;
        for (unsigned i = 0; i < 24 * 35; i++)
            pixels[i] = shadow[i] = untouched;
        renderer_glass_background_locked(0x123456);
        for (unsigned y = 0; y < 35; y++) {
            for (unsigned x = 0; x < 24; x++) {
                bool margin = x < 19 && (x >= 16 || y >= 32);
                unsigned i = y * 24 + x;
                check(pixels[i] == (margin && mode != 1 ? 0x123456 : untouched));
                check(shadow[i] == (margin && mode != 2 ? 0x123456 : untouched));
            }
        }
    }
    check(kFrameBufferBackgroundColor == 0x123456);
    puts("test_ansi_margins_host: all checks passed");
    return 0;
}
