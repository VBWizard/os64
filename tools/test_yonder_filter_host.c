// yonder's filtered groups (userland/apps/yonder/filter.c; FILTER.md) on the
// host. The colour cases are WPT's css/filter-effects reftests' own
// expectations (each named); the rest are worked by hand from the rules in
// filter.h, or held to a reference written apart from the code.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "filter.h"

static int failures, checks;

static void expect(const char *what, int ok, const char *detail)
{
    checks++;
    if (!ok) {
        failures++;
        printf("FAIL %s%s%s\n", what, detail != NULL ? ": " : "", detail != NULL ? detail : "");
    }
}

static flow_filter_t fn(flow_filter_fn_t which, double amount)
{
    flow_filter_t f;
    memset(&f, 0, sizeof(f));
    f.fn = which;
    f.amount = amount;
    return f;
}

// One opaque pixel of `rgb` through one function: its RGB after.
static uint32_t one(uint32_t rgb, flow_filter_t f)
{
    uint32_t px = 0xff000000u | rgb;
    void *work = malloc(yonder_filter_work_bytes(1, 1, &f, 1));
    yonder_filter_run(&px, 1, 1, &f, 1, 0, work);
    free(work);
    return px;
}

static void colour_cases(void)
{
    static const struct {
        const char *test;
        flow_filter_fn_t fn;
        double amount;
        uint32_t in, want;
    } kCases[] = {
        {"filter-contrast-001", FLOW_FILTER_CONTRAST, 1, 0x008000, 0x008000},
        {"filter-contrast-002", FLOW_FILTER_CONTRAST, 0, 0xff0000, 0x808080},
        {"filter-contrast-003", FLOW_FILTER_CONTRAST, 2, 0x3f0000, 0x000000},
        {"filter-grayscale-001", FLOW_FILTER_GRAYSCALE, 1, 0x0000ff, 0x121212},
        {"filter-grayscale-002", FLOW_FILTER_GRAYSCALE, 0, 0x121212, 0x121212},
        {"filter-hue_rotate-001", FLOW_FILTER_HUE_ROTATE, 0, 0x008000, 0x008000},
        {"filter-hue_rotate-002", FLOW_FILTER_HUE_ROTATE, 120, 0xff0000, 0x007100},
        {"filter-invert-001", FLOW_FILTER_INVERT, 0, 0xffff00, 0xffff00},
        {"filter-invert-002", FLOW_FILTER_INVERT, 1, 0xffff00, 0x0000ff},
        {"filter-saturate-001", FLOW_FILTER_SATURATE, 0, 0xffff00, 0xededed},
    };
    for (size_t i = 0; i < sizeof(kCases) / sizeof(kCases[0]); i++) {
        uint32_t got = one(kCases[i].in, fn(kCases[i].fn, kCases[i].amount));
        char detail[96];
        snprintf(detail, sizeof(detail), "got %08x, want ff%06x", got, kCases[i].want);
        char what[96];
        snprintf(what, sizeof(what), "colour: WPT %s", kCases[i].test);
        expect(what, got == (0xff000000u | kCases[i].want), detail);
    }
    // brightness is a slope on each channel, held at 1.
    expect("colour: brightness(0.5) halves", one(0xc86432, fn(FLOW_FILTER_BRIGHTNESS, 0.5)) == 0xff643219,
           NULL);
    expect("colour: brightness(2) holds at 1", one(0x80ff00, fn(FLOW_FILTER_BRIGHTNESS, 2)) == 0xffffff00,
           NULL);
    // Each function is held before the next: brightness(2) then
    // brightness(0.5) is not the identity on a bright channel.
    flow_filter_t two[2] = {fn(FLOW_FILTER_BRIGHTNESS, 2), fn(FLOW_FILTER_BRIGHTNESS, 0.5)};
    uint32_t px = 0xffc80000u;
    void *work = malloc(yonder_filter_work_bytes(1, 1, two, 2));
    yonder_filter_run(&px, 1, 1, two, 2, 0, work);
    free(work);
    expect("colour: each function is held before the next", px == 0xff800000u, NULL);
    // opacity scales the coverage, and the colour with it.
    expect("colour: opacity(0.5) halves the coverage", one(0xffffff, fn(FLOW_FILTER_OPACITY, 0.5)) == 0x80808080u,
           NULL);
    // A half-covered pixel is filtered unpremultiplied: pure red at 128 is
    // red to grayscale, 0.2126 of it, still at 128.
    uint32_t half = 0x80800000u;
    flow_filter_t gray = fn(FLOW_FILTER_GRAYSCALE, 1);
    work = malloc(yonder_filter_work_bytes(1, 1, &gray, 1));
    yonder_filter_run(&half, 1, 1, &gray, 1, 0, work);
    free(work);
    uint32_t g = (uint32_t)(0.2126 * 128.0 / 255.0 * 255.0 + 0.5);
    char detail[64];
    snprintf(detail, sizeof(detail), "got %08x", half);
    expect("colour: a half-covered pixel is filtered unpremultiplied",
           half == (0x80000000u | g << 16 | g << 8 | g), detail);
}

static void recover_cases(void)
{
    // Opaque, half red over each backdrop, untouched.
    uint32_t black[3] = {0x336699, 0x800000, 0x000000};
    uint32_t white[3] = {0x336699, 0xff7f7f, 0xffffff};
    yonder_sheets_t s = {black, white, 3, 1};
    uint32_t out[3];
    yonder_filter_recover(&s, out);
    expect("recover: an opaque pixel is itself", out[0] == 0xff336699u, NULL);
    expect("recover: a half-covered red", out[1] == 0x80800000u, NULL);
    expect("recover: an untouched pixel is nothing", out[2] == 0, NULL);
}

// What a pixel painted at coverage a in colour c over black and white
// comes back as, laid over D: c a + D (1 - a), to a step.
static void round_trip_cases(void)
{
    int worst = 0;
    for (uint32_t a = 0; a <= 255; a += 15)
        for (uint32_t c = 0; c <= 255; c += 51)
            for (uint32_t d = 0; d <= 255; d += 85) {
                uint32_t b = (c * a + 127) / 255, w = b + ((255 - a) * 255 + 127) / 255;
                uint32_t bl = b << 16 | b << 8 | b, wh = w << 16 | w << 8 | w;
                yonder_sheets_t s = {&bl, &wh, 1, 1};
                uint32_t layer;
                yonder_filter_recover(&s, &layer);
                uint32_t dst = 0xff000000u | d << 16 | d << 8 | d;
                yonder_filter_lay(&dst, 1, (os64_gui_rect_t){0, 0, 1, 1}, &layer, 1, 1, 0, 0, 255);
                int want = (int)((c * a + d * (255 - a) + 127) / 255);
                int off = abs((int)(dst & 0xff) - want);
                worst = off > worst ? off : worst;
            }
    char detail[32];
    snprintf(detail, sizeof(detail), "worst %d", worst);
    expect("round trip: painted, recovered and laid is painted over it, to a step", worst <= 1,
           detail);
}

static void lay_cases(void)
{
    // Half red over white at full opacity, and at half.
    uint32_t layer = 0x80800000u;
    uint32_t dst = 0xffffffffu;
    yonder_filter_lay(&dst, 1, (os64_gui_rect_t){0, 0, 1, 1}, &layer, 1, 1, 0, 0, 255);
    expect("lay: half red over white", dst == 0xffff7f7fu, NULL);
    dst = 0xffffffffu;
    yonder_filter_lay(&dst, 1, (os64_gui_rect_t){0, 0, 1, 1}, &layer, 1, 1, 0, 0, 128);
    // c alpha + d (1 - a alpha): red 128 * 128/255 + 255 (1 - 128/255 *
    // 128/255) = 64.25 + 190.75; green 0 + 190.75.
    char detail[32];
    snprintf(detail, sizeof(detail), "got %08x", dst);
    expect("lay: half red over white at half", dst == 0xffffbfbfu, detail);
    // Only inside the clip, moved by the offset, the top byte kept.
    uint32_t grid[4] = {0x01000000u, 0x02000000u, 0x03000000u, 0x04000000u};
    uint32_t opaque = 0xff00ff00u;
    yonder_filter_lay(grid, 2, (os64_gui_rect_t){1, 0, 1, 2}, &opaque, 1, 1, 1, 1, 255);
    yonder_filter_lay(grid, 2, (os64_gui_rect_t){1, 0, 1, 2}, &opaque, 1, 1, 0, 1, 255);
    expect("lay: inside the clip, at the offset, the top byte kept",
           grid[0] == 0x01000000u && grid[1] == 0x02000000u && grid[2] == 0x03000000u &&
               grid[3] == 0x0400ff00u, NULL);
}

static void blur_cases(void)
{
    // A 20 x 20 opaque white square in the middle of a 60 x 60 layer,
    // blurred at a deviation of 2: three box blurs of radius 2, reaching 6,
    // so the middle is more than a reach from every edge.
    enum { W = 60, S0 = 20, S1 = 40 };
    uint32_t *px = calloc(W * W, sizeof(uint32_t));
    for (int y = S0; y < S1; y++)
        for (int x = S0; x < S1; x++)
            px[y * W + x] = 0xffffffffu;
    flow_filter_t f = fn(FLOW_FILTER_BLUR, 2);
    f.shadow.blur = 4;
    void *work = malloc(yonder_filter_work_bytes(W, W, &f, 1));
    yonder_filter_run(px, W, W, &f, 1, 0, work);
    free(work);
    int mid = (S0 + S1) / 2;
    bool symmetric = true, premultiplied = true;
    for (int y = 0; y < W; y++)
        for (int x = 0; x < W; x++) {
            uint32_t p = px[y * W + x], q = px[y * W + (S0 + S1 - 1 - x)];
            uint32_t r = px[(S0 + S1 - 1 - y) * W + x];
            symmetric &= p == q && p == r;
            premultiplied &= ((p >> 16) & 0xff) <= p >> 24;
        }
    expect("blur: symmetric", symmetric, NULL);
    expect("blur: every pixel premultiplied", premultiplied, NULL);
    expect("blur: the middle stays covered", px[mid * W + mid] == 0xffffffffu, NULL);
    expect("blur: reaches 3 deviations past the edge and no further",
           px[mid * W + S1 - 1 + 6] != 0 && px[mid * W + S1 - 1 + 7] == 0 && px[mid * W + S0 - 7] == 0,
           NULL);
    uint32_t edge = px[mid * W + S1] >> 24, inside = px[mid * W + S1 - 1] >> 24;
    char detail[48];
    snprintf(detail, sizeof(detail), "inside %u outside %u", inside, edge);
    expect("blur: the edge fades through half to transparent",
           inside > 128 && edge < 128 && inside + edge >= 250 && inside + edge <= 260, detail);
    uint64_t sum = 0;
    for (int i = 0; i < W * W; i++)
        sum += px[i] >> 24;
    snprintf(detail, sizeof(detail), "sum %lu", (unsigned long)sum);
    expect("blur: coverage is kept, to rounding", sum >= 400 * 255 - 800 && sum <= 400 * 255 + 800,
           detail);
    free(px);
}

static void shadow_cases(void)
{
    // A 4 x 4 opaque white square at (2, 2) of a 12 x 12 layer, its shadow
    // cast 3 right, unblurred, in red at half: it shows only where the
    // square does not cover.
    enum { W = 12 };
    uint32_t px[W * W];
    memset(px, 0, sizeof(px));
    for (int y = 2; y < 6; y++)
        for (int x = 2; x < 6; x++)
            px[y * W + x] = 0xffffffffu;
    flow_filter_t f = fn(FLOW_FILTER_DROP_SHADOW, 0);
    f.shadow.x = 3;
    f.shadow.colour = 0x7fff0000u;     // flow's: 127 transparent, so 128 of it there
    void *work = malloc(yonder_filter_work_bytes(W, W, &f, 1));
    yonder_filter_run(px, W, W, &f, 1, 0, work);
    expect("drop-shadow: the layer stays over its shadow", px[3 * W + 4] == 0xffffffffu, NULL);
    char detail[48];
    snprintf(detail, sizeof(detail), "got %08x", px[3 * W + 7]);
    expect("drop-shadow: cast by its offset, in its colour", px[3 * W + 7] == 0x80800000u, detail);
    expect("drop-shadow: nothing where it does not fall",
           px[3 * W + 1] == 0 && px[3 * W + 9] == 0 && px[1 * W + 7] == 0, NULL);
    // In currentColor: the colour handed over.
    memset(px, 0, sizeof(px));
    px[3 * W + 3] = 0xffffffffu;
    f.shadow.current = true;
    yonder_filter_run(px, W, W, &f, 1, 0x000000ffu, work);
    expect("drop-shadow: currentColor is the colour handed over", px[3 * W + 6] == 0xff0000ffu, NULL);
    free(work);
}

int main(void)
{
    colour_cases();
    recover_cases();
    round_trip_cases();
    lay_cases();
    blur_cases();
    shadow_cases();
    printf("yonder filter: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
