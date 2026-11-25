// planetest — a window's plane from ring 3 (docs/design/pending/
// VIDEO_PLANE.md § 3): a picture thread publishing a moving pattern on a
// timer (bars moving left, a band moving up), under a see-through hole the
// window's thread places with its own publish.
//
//   - the picture thread creates the plane and is its creator: the window's
//     thread is refused its surface, its publish and its destroy;
//   - damage short of the hole, an overlay outside its clip and an unknown
//     name are refused (that a refusal changes nothing is the kernel's own
//     test, kernel/test/test_planes.c, which can see the plane's state);
//   - placed, the plane is drawn: the compositor's counts rise;
//   - WHAT IS ON THE GLASS is the rule (§ 3.3): at the end of each phase the
//     picture thread holds still on a known frame and every pixel of the
//     hole is read back through /dev/glass and compared with the picture,
//     the content, and the blend worked out again here, channel by channel.
//     The pointer is part of the glass, so mismatches confined to a box the
//     pointer's size are let pass and counted;
//   - placed, the plane cannot be destroyed, even by its creator, nor can
//     its window;
//   - the picture thread ends with its plane placed, and the plane goes
//     with it: the window can then be destroyed;
//   - a program that dies with a plane placed gives its pair of stores
//     back when it is buried: /sys/gui's count of pairs comes back down.
//
// And it prices the compositor's part (/sys/gui's plane line: what was
// drawn, and in how long) in three phases: no overlay (every pixel a picture
// pixel copied), an overlay strip whose content pixels are translucent (each
// one blended), and the same strip with content pixels that are wholly
// opaque or wholly see-through (each one read and found to need no blend,
// which is what a scrim mostly is). Before them, with the plane hidden, the
// window repaints the same hole itself at the same rate, so a plane's
// copied pixel can be set beside the content pixel it replaces: both are
// priced the same way, the compositor's hold over the pixels it drew.
// Each phase also prints the worst hold of
// kGuiLock by the plane, publish and composite sites: /sys/gui's worst holds
// start again at every read, and this fixture's own reads bracket a phase.
//
// `planetest --watch` is the same for a person's eye: a larger picture, five
// seconds a phase, squares of 32 pixels and a strip of 96 rows, and each
// phase says what should be on the glass as it begins.
//
// A covered window is drawn nothing, so the fixture SKIPS when its window is
// covered rather than pricing and checking nothing.

#include "os64/os64.h"
#include "os64/gui.h"
#include "os64/draw.h"
#include "os64/glass.h"
#include "os64/thread.h"

#define PLANE_OK   0x91A40000
#define PLANE_FAIL 0x91A40001
#define PLANE_SKIP 0x91A40002

// The pointer is drawn on the glass; mismatches inside a box this size are
// taken to be it.
#define POINTER_BOX 40

typedef struct {
    uint32_t content_w, content_h, pic_w, pic_h, hole_x, hole_y, strip_h, square, phase_ms;
} layout_t;

static const layout_t kTest = {640, 400, 480, 270, 80, 40, 48, 8, 1000};
static const layout_t kWatch = {800, 520, 640, 360, 80, 60, 96, 32, 5000};
static const layout_t *L = &kTest;

static int failures;
#define CHECK(c) do { if (!(c)) { os64_printf("planetest: FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

static int64_t s_win;
static volatile uint32_t s_plane;      // the picture thread's plane, once made
static volatile int s_pictured;        // its first picture is published
static volatile int s_stop;            // the window's thread says: end
static volatile uint64_t s_published;
static volatile int s_try_destroy;     // the window's thread asks the creator to try
static volatile int64_t s_destroy_rc;  // and what it was told
static volatile int s_hold;            // the window's thread says: hold still
static volatile int s_held;            // the picture thread is holding still
static volatile uint32_t s_frame;      // the frame on the plane's front

// The picture: bars moving left, a band moving up, so a misplaced or stale
// row shows on the glass.
static uint32_t picture_px(uint32_t x, uint32_t y, uint32_t frame)
{
    uint32_t band = ((y + frame * 2) / 30) & 1 ? 0x00303030u : 0;
    return ((((x + frame * 4) / 40) & 1) ? 0x00C04020u : 0x002060C0u) + band;
}

static int64_t picture_thread(void *arg)
{
    (void)arg;
    int64_t name = os64_gui_plane_create(s_win, L->pic_w, L->pic_h);
    if (name <= 0) {
        os64_printf("planetest: plane_create answered %ld\n", name);
        s_pictured = -1;
        return 1;
    }
    os64_gui_plane_surface_t s;
    if (os64_gui_plane_surface(s_win, (uint32_t)name, &s) != 0) {
        s_pictured = -1;
        return 1;
    }
    s_plane = (uint32_t)name;
    for (uint32_t frame = 0; !s_stop;) {
        if (s_hold) {
            s_held = 1;
            os64_sleep(5);
            continue;
        }
        for (uint32_t y = 0; y < L->pic_h; y++) {
            uint32_t *row = s.pixels + (size_t)y * s.pitch_px;
            for (uint32_t x = 0; x < L->pic_w; x++)
                row[x] = picture_px(x, y, frame);
        }
        if (os64_gui_plane_publish(s_win, (uint32_t)name, L->pic_w, L->pic_h) == 0) {
            s_published++;
            s_frame = frame;
        }
        frame++;
        s_pictured = 1;
        if (s_try_destroy == 1) {
            s_destroy_rc = os64_gui_plane_destroy(s_win, (uint32_t)name);
            s_try_destroy = 2;
        }
        os64_sleep(16);
    }
    // Ending with the plane placed: the kernel takes it out.
    return 0;
}

// The window's content: opaque everywhere but the hole, which is wholly
// see-through but for the strip along its foot, painted by `strip`.
enum { STRIP_NONE, STRIP_TRANSLUCENT, STRIP_OPAQUE_OR_CLEAR };

static bool in_hole(uint32_t x, uint32_t y)
{
    return x >= L->hole_x && x < L->hole_x + L->pic_w && y >= L->hole_y && y < L->hole_y + L->pic_h;
}

static bool in_strip(uint32_t x, uint32_t y)
{
    return in_hole(x, y) && y >= L->hole_y + L->pic_h - L->strip_h;
}

static uint32_t content_px(uint32_t x, uint32_t y, int strip)
{
    if (in_strip(x, y) && strip == STRIP_TRANSLUCENT)
        return 0x80202020u;   // half covering, premultiplied
    if (in_strip(x, y) && strip == STRIP_OPAQUE_OR_CLEAR)
        return ((x / L->square + y / L->square) & 1) ? 0xFFE0E0E0u : 0x00000000u;
    return in_hole(x, y) ? 0 : 0xFF283038u;
}

static void paint_content(os64_draw_ctx_t *ctx, int strip)
{
    os64_gui_surface_t *c = &ctx->surf;
    for (uint32_t y = 0; y < c->height; y++) {
        uint32_t *row = c->pixels + (size_t)y * c->pitch_px;
        for (uint32_t x = 0; x < c->width; x++)
            row[x] = content_px(x, y, strip);
    }
}

static os64_gui_plane_edit_t place(uint32_t plane, int strip)
{
    os64_gui_plane_edit_t e = {0};
    e.plane = plane;
    e.origin = (os64_gui_rect_t){(int32_t)L->hole_x, (int32_t)L->hole_y, (int32_t)L->pic_w, (int32_t)L->pic_h};
    e.clip = e.origin;
    if (strip != STRIP_NONE) {
        e.noverlays = 1;
        e.overlay[0] = (os64_gui_rect_t){(int32_t)L->hole_x, (int32_t)(L->hole_y + L->pic_h - L->strip_h),
                                         (int32_t)L->pic_w, (int32_t)L->strip_h};
    }
    return e;
}

// ── What the glass should show ──────────────────────────────────────────────

// The rule (§ 3.3), channel by channel, as the design writes it: 255 the
// content, 0 the picture, between them the content plus the picture scaled
// by what the content leaves, rounded to nearest, held at 255.
static uint32_t rule_blend(uint32_t c, uint32_t p)
{
    uint32_t a = c >> 24, out = 0;
    if (a == 255)
        return c;
    if (a == 0)
        return p;
    for (int s = 0; s < 32; s += 8) {
        uint32_t v = ((c >> s) & 0xFF) + (((p >> s) & 0xFF) * (255 - a) + 127) / 255;
        out |= (v > 255 ? 255 : v) << s;
    }
    return out;
}

// A content pixel in the hole, on the glass: the picture where the content
// is see-through, the rule inside the strip's overlay.
static uint32_t expected(uint32_t x, uint32_t y, int strip, uint32_t frame)
{
    uint32_t p = picture_px(x - L->hole_x, y - L->hole_y, frame);
    if (strip != STRIP_NONE && in_strip(x, y))
        return rule_blend(content_px(x, y, strip), p);
    return p;
}

static int64_t s_glass = -1;
static uint32_t *s_screen;             // the glass as last read
static uint32_t s_screen_w, s_screen_h;
static uint8_t s_record[1 << 20];

// Every change the glass has to give, into s_screen, until it has been
// still for a moment (or two seconds have gone: something else on the
// desktop may never be still).
static bool glass_catch_up(void)
{
    int64_t until = os64_micros() + 2000000;
    while (os64_micros() < until) {
        int64_t n = os64_read_for((int32_t)s_glass, s_record, sizeof(s_record), 150);
        if (n == OS64_ERR_TIMEOUT)
            return true;
        if (n < (int64_t)sizeof(os64_glass_rect_t))
            return false;
        os64_glass_rect_t head;
        os64_memcpy(&head, s_record, sizeof(head));
        if (head.screen_w != s_screen_w || head.screen_h != s_screen_h ||
            (uint32_t)head.x + head.w > s_screen_w || (uint32_t)head.y + head.h > s_screen_h ||
            (uint64_t)n != sizeof(head) + (uint64_t)head.w * head.h * 4)
            return false;
        const uint32_t *px = (const uint32_t *)(s_record + sizeof(head));
        for (uint32_t r = 0; r < head.h; r++)
            os64_memcpy(s_screen + (size_t)(head.y + r) * s_screen_w + head.x, px + (size_t)r * head.w,
                        (size_t)head.w * 4);
    }
    return true;
}

static uint32_t on_glass(int32_t sx, int32_t sy)
{
    if (sx < 0 || sy < 0 || (uint32_t)sx >= s_screen_w || (uint32_t)sy >= s_screen_h)
        return 0xFFFFFFFFu;
    return s_screen[(size_t)sy * s_screen_w + (uint32_t)sx] & 0x00FFFFFFu;
}

static void sample(const char *what, os64_gui_rect_t cr, uint32_t x, uint32_t y, int strip, uint32_t frame)
{
    os64_printf("planetest:   %s (%u,%u): glass %06x, the rule %06x\n", what, x, y,
                on_glass(cr.x + (int32_t)x, cr.y + (int32_t)y), expected(x, y, strip, frame) & 0x00FFFFFFu);
}

// With the picture held still: every pixel of the hole on the glass against
// the rule. False on mismatches the pointer cannot explain.
static bool glass_check(int strip, const char *phase_name)
{
    s_held = 0;
    s_hold = 1;
    for (int i = 0; i < 400 && !s_held; i++)
        os64_sleep(5);
    bool ok = s_held != 0;
    uint32_t frame = s_frame;
    os64_gui_rect_t cr;
    ok = ok && os64_gui_window_get_content_rect(s_win, &cr) == 0 && glass_catch_up();
    uint64_t bad = 0, checked = 0;
    int32_t bx0 = INT32_MAX, by0 = INT32_MAX, bx1 = -1, by1 = -1;
    uint32_t fx = 0, fy = 0;
    for (uint32_t y = L->hole_y; ok && y < L->hole_y + L->pic_h; y++) {
        for (uint32_t x = L->hole_x; x < L->hole_x + L->pic_w; x++) {
            int32_t sx = cr.x + (int32_t)x, sy = cr.y + (int32_t)y;
            if (sx < 0 || sy < 0 || (uint32_t)sx >= s_screen_w || (uint32_t)sy >= s_screen_h)
                continue;
            checked++;
            if (on_glass(sx, sy) == (expected(x, y, strip, frame) & 0x00FFFFFFu))
                continue;
            if (bad++ == 0) {
                fx = x;
                fy = y;
            }
            bx0 = sx < bx0 ? sx : bx0;
            by0 = sy < by0 ? sy : by0;
            bx1 = sx > bx1 ? sx : bx1;
            by1 = sy > by1 ? sy : by1;
        }
    }
    bool pointer = bad > 0 && bx1 - bx0 < POINTER_BOX && by1 - by0 < POINTER_BOX;
    os64_printf("planetest: %s on the glass at frame %u: %lu pixels checked, %lu differ%s\n", phase_name, frame,
                checked, bad, bad == 0 ? "" : pointer ? " (inside one pointer-sized box: taken as the pointer)" : "");
    if (ok) {
        uint32_t mid_x = L->hole_x + L->pic_w / 2, strip_y = L->hole_y + L->pic_h - L->strip_h / 2;
        sample("the hole", cr, mid_x, L->hole_y + 10, strip, frame);
        if (strip != STRIP_NONE) {
            sample("the strip", cr, mid_x, strip_y, strip, frame);
            sample("the strip", cr, mid_x + L->square, strip_y, strip, frame);
        }
        if (bad > 0 && !pointer)
            sample("the first that differs", cr, fx, fy, strip, frame);
    }
    s_held = 0;
    s_hold = 0;
    return ok && checked > 0 && (bad == 0 || pointer);
}

// ── /sys/gui ────────────────────────────────────────────────────────────────

static char s_text[32768];

static bool find(const char *hay, const char *needle, const char **at)
{
    for (const char *h = hay; *h; h++) {
        const char *a = h, *b = needle;
        while (*a && *b && *a == *b) {
            a++;
            b++;
        }
        if (*b == 0) {
            *at = a;
            return true;
        }
    }
    return false;
}

static uint64_t number(const char **at)
{
    uint64_t v = 0;
    while (**at == ' ')
        (*at)++;
    while (**at >= '0' && **at <= '9')
        v = v * 10 + (uint64_t)(*(*at)++ - '0');
    return v;
}

// /sys/gui into s_text. Every read starts the worst holds again.
static bool read_gui(void)
{
    int64_t h = os64_open("/sys/gui", "r");
    if (h < 0)
        return false;
    int64_t n = 0, got;
    while (n < (int64_t)sizeof(s_text) - 1 && (got = os64_read((int32_t)h, s_text + n, sizeof(s_text) - 1 - (size_t)n)) > 0)
        n += got;
    os64_close((int32_t)h);
    s_text[n] = 0;
    return true;
}

// A lock site's worst hold since the last read, in microseconds.
static uint64_t worst_hold(const char *site)
{
    char key[32];
    os64_snprintf(key, sizeof(key), "lock %s:", site);
    const char *at;
    if (!find(s_text, key, &at) || !find(at, "worst hold", &at))
        return 0;
    return number(&at);
}

// This window's plane line: what was composed, and the time.
typedef struct {
    uint64_t copied, blended, opaque, ns;
} composed_t;

static bool composed(uint32_t plane, composed_t *out)
{
    char key[64];
    os64_snprintf(key, sizeof(key), "plane %u of window %ld:", plane, s_win);
    const char *at;
    if (!find(s_text, key, &at) || !find(at, "composed ", &at))
        return false;
    out->copied = number(&at);
    if (!find(at, "copied", &at))
        return false;
    out->blended = number(&at);
    if (!find(at, "blended", &at))
        return false;
    out->opaque = number(&at);
    if (!find(at, "in", &at))
        return false;
    out->ns = number(&at);
    return true;
}

// The compositor's work so far: the pixels it drew and its hold of kGuiLock.
typedef struct {
    uint64_t pixels, hold_us;
} compositor_t;

static bool compositor(compositor_t *out)
{
    const char *at;
    if (!find(s_text, "compositor:", &at) || !find(at, "rectangles,", &at))
        return false;
    out->pixels = number(&at);
    if (!find(s_text, "lock composite:", &at) || !find(at, "us waiting,", &at))
        return false;
    out->hold_us = number(&at);
    return true;
}

// Picoseconds a pixel drawn, from two readings of the compositor.
static uint64_t compositor_ps(const compositor_t *a, const compositor_t *b)
{
    uint64_t px = b->pixels - a->pixels;
    return px ? (b->hold_us - a->hold_us) * 1000000 / px : 0;
}

static bool read_pairs(uint64_t *pairs)
{
    const char *at;
    if (!read_gui() || !find(s_text, "pairs of stores", &at))
        return false;
    *pairs = number(&at);
    return true;
}

// ── The phases ──────────────────────────────────────────────────────────────

static const char *const kPhaseNames[] = {"no overlay", "a translucent strip", "a strip opaque or clear"};
static const char *const kPhaseSights[] = {
    "the whole hole is the picture: bars moving left, a band moving up",
    "a strip along the hole's foot darkens the picture, which moves on through it",
    "the strip is light squares and clear ones, the picture moving behind the clear ones",
};

// One phase: placed with `strip`, its time of pictures, the counts' rise
// and the worst holds, then the glass checked.
static compositor_t s_comp_a, s_comp_b;   // the last phase's two readings

static bool phase(os64_draw_ctx_t *ctx, uint32_t plane, int strip, composed_t *rise)
{
    if (L == &kWatch)
        os64_printf("planetest: phase %d, %s: %s\n", strip + 1, kPhaseNames[strip], kPhaseSights[strip]);
    paint_content(ctx, strip);
    os64_gui_plane_edit_t e = place(plane, strip);
    int64_t rc = os64_gui_window_publish_edits(s_win, NULL, &e, 1);
    CHECK(rc == 0);
    composed_t a, b;
    if (rc != 0 || !read_gui() || !composed(plane, &a) || !compositor(&s_comp_a))
        return false;
    os64_sleep(L->phase_ms);
    if (!read_gui() || !composed(plane, &b) || !compositor(&s_comp_b))
        return false;
    *rise = (composed_t){b.copied - a.copied, b.blended - a.blended, b.opaque - a.opaque, b.ns - a.ns};
    os64_printf("planetest: %s: worst holds of kGuiLock: plane %lu us, publish %lu us, composite %lu us\n",
                kPhaseNames[strip], worst_hold("plane"), worst_hold("publish"), worst_hold("composite"));
    bool seen = glass_check(strip, kPhaseNames[strip]);
    CHECK(seen);
    return true;
}

// The plane hidden, the window repaints the hole itself with the same
// pattern at the picture thread's rate: the content's price, through the
// compositor's ordinary blit.
static bool content_phase(os64_draw_ctx_t *ctx, uint64_t *ps)
{
    compositor_t a, b;
    os64_gui_rect_t hole = {(int32_t)L->hole_x, (int32_t)L->hole_y, (int32_t)L->pic_w, (int32_t)L->pic_h};
    if (!read_gui() || !compositor(&a))
        return false;
    int64_t until = os64_micros() + (int64_t)L->phase_ms * 1000;
    for (uint32_t frame = 0; os64_micros() < until; frame++) {
        os64_gui_surface_t *c = &ctx->surf;
        for (uint32_t y = 0; y < L->pic_h; y++) {
            uint32_t *row = c->pixels + (size_t)(L->hole_y + y) * c->pitch_px + L->hole_x;
            for (uint32_t x = 0; x < L->pic_w; x++)
                row[x] = 0xFF000000u | picture_px(x, y, frame);
        }
        os64_gui_window_publish(s_win, &hole);
        os64_sleep(16);
    }
    if (!read_gui() || !compositor(&b))
        return false;
    *ps = compositor_ps(&a, &b);
    return true;
}

// The orphan: a window and a placed plane, and an exit that cleans up
// nothing. 0 when it got as far as the exit with its plane placed.
static int orphan(void)
{
    s_win = os64_gui_window_create("planetest orphan", 80, 80, 320, 200, OS64_GUI_CREATE_CONTENT_SIZE);
    if (s_win <= 0)
        return 1;
    int64_t name = os64_gui_plane_create(s_win, 64, 48);
    os64_gui_plane_surface_t s;
    if (name <= 0 || os64_gui_plane_surface(s_win, (uint32_t)name, &s) != 0 ||
        os64_gui_plane_publish(s_win, (uint32_t)name, 64, 48) != 0)
        return 2;
    os64_gui_plane_edit_t e = {0};
    e.plane = (uint32_t)name;
    e.origin = (os64_gui_rect_t){10, 10, 64, 48};
    e.clip = e.origin;
    return os64_gui_window_publish_edits(s_win, NULL, &e, 1) == 0 ? 0 : 3;
}

// Picoseconds a pixel, printed as nanoseconds with two places.
static void print_ps(const char *what, uint64_t ps)
{
    os64_printf("planetest: %s %lu.%02lu ns a pixel\n", what, ps / 1000, (ps % 1000) / 10);
}

int main(int argc, char **argv)
{
    if (argc == 2 && os64_streq(argv[1], "--orphan"))
        return orphan();
    if (argc == 2 && os64_streq(argv[1], "--watch"))
        L = &kWatch;

    s_win = os64_gui_window_create("planetest", 60, 60, L->content_w, L->content_h, OS64_GUI_CREATE_CONTENT_SIZE);
    if (s_win == OS64_GUI_ERR_NOT_RUNNING) {
        os64_printf("planetest: SKIP (requires GUI)\n");
        return PLANE_SKIP;
    }
    CHECK(s_win > 0);
    if (s_win <= 0)
        return PLANE_FAIL;
    os64_draw_ctx_t ctx;
    CHECK(os64_draw_ctx_init(&ctx, s_win) == 0);
    paint_content(&ctx, STRIP_NONE);
    os64_gui_window_publish(s_win, NULL);
    os64_sleep(100);   // the cover sweep's verdict on the new window
    os64_gui_window_state_t st;
    if (os64_gui_window_get_state(s_win, &st) == 0 && (st.flags & OS64_GUI_WINDOW_COVERED)) {
        os64_printf("planetest: SKIP (its window is covered: nothing would be drawn)\n");
        os64_gui_window_destroy(s_win);
        return PLANE_SKIP;
    }
    os64_gui_screen_info(&s_screen_w, &s_screen_h);
    s_screen = os64_malloc((size_t)s_screen_w * s_screen_h * 4);
    s_glass = os64_open("/dev/glass", "r");
    CHECK(s_screen != NULL && s_glass >= 0);
    if (s_screen == NULL || s_glass < 0) {
        os64_gui_window_destroy(s_win);
        return PLANE_FAIL;
    }

    int64_t th = os64_thread(picture_thread, NULL);
    CHECK(th > 0);
    for (int i = 0; i < 200 && s_pictured == 0; i++)
        os64_sleep(5);
    CHECK(s_pictured == 1);
    if (th <= 0 || s_pictured != 1) {
        s_stop = 1;
        if (th > 0)
            os64_thread_join((int32_t)th, NULL);
        os64_gui_window_destroy(s_win);
        return PLANE_FAIL;
    }
    uint32_t plane = s_plane;

    // The creator rule, from the window's thread.
    os64_gui_plane_surface_t s;
    CHECK(os64_gui_plane_surface(s_win, plane, &s) == OS64_GUI_ERR_NOT_CREATOR);
    CHECK(os64_gui_plane_publish(s_win, plane, L->pic_w, L->pic_h) == OS64_GUI_ERR_NOT_CREATOR);
    CHECK(os64_gui_plane_destroy(s_win, plane) == OS64_GUI_ERR_NOT_CREATOR);

    // Refused commits.
    os64_gui_rect_t short_damage = {0, 0, 40, 40};
    os64_gui_plane_edit_t e = place(plane, STRIP_NONE);
    CHECK(os64_gui_window_publish_edits(s_win, &short_damage, &e, 1) == OS64_GUI_ERR_PLANE_DAMAGE);
    os64_gui_plane_edit_t out = place(plane, STRIP_TRANSLUCENT);
    out.overlay[0].x = 0;
    CHECK(os64_gui_window_publish_edits(s_win, NULL, &out, 1) == OS64_GUI_ERR_PLANE_OVERLAP);
    os64_gui_plane_edit_t bogus = place(plane + 1, STRIP_NONE);
    CHECK(os64_gui_window_publish_edits(s_win, NULL, &bogus, 1) == OS64_GUI_ERR_NO_PLANE);

    uint64_t content_ps = 0;
    bool content_priced = content_phase(&ctx, &content_ps);
    CHECK(content_priced);
    composed_t copy = {0}, blend = {0}, read = {0};
    bool priced = phase(&ctx, plane, STRIP_NONE, &copy);
    CHECK(priced && copy.copied > 0);
    uint64_t plane_ps = priced ? compositor_ps(&s_comp_a, &s_comp_b) : 0;
    // While placed: neither the window nor the plane may go, the plane not
    // even at its creator's asking.
    CHECK(os64_gui_window_destroy(s_win) == OS64_GUI_ERR_BUSY);
    s_try_destroy = 1;
    for (int i = 0; i < 200 && s_try_destroy != 2; i++)
        os64_sleep(5);
    CHECK(s_try_destroy == 2 && s_destroy_rc == OS64_GUI_ERR_BUSY);
    priced = priced && phase(&ctx, plane, STRIP_TRANSLUCENT, &blend);
    CHECK(priced && blend.blended > 0);
    priced = priced && phase(&ctx, plane, STRIP_OPAQUE_OR_CLEAR, &read);
    CHECK(priced && read.blended > 0);

    // The picture thread ends with its plane placed: the plane goes with it.
    s_stop = 1;
    os64_thread_join((int32_t)th, NULL);
    CHECK(os64_gui_plane_publish(s_win, plane, L->pic_w, L->pic_h) == OS64_GUI_ERR_NO_PLANE);

    if (priced && copy.copied > 0) {
        uint64_t copy_ps = copy.ns * 1000 / copy.copied;
        os64_printf("planetest: %lu pictures published; composed %lu copied, %lu blended, %lu opaque\n",
                    s_published, copy.copied + blend.copied + read.copied, blend.blended + read.blended,
                    copy.opaque + blend.opaque + read.opaque);
        print_ps("copied (no overlay):", copy_ps);
        if (content_priced) {
            print_ps("through the compositor's hold, the window's content repainted:", content_ps);
            print_ps("through the compositor's hold, the plane copied:", plane_ps);
        }
        if (blend.blended > 0) {
            uint64_t rest = blend.ns * 1000 > blend.copied * copy_ps ? blend.ns * 1000 - blend.copied * copy_ps : 0;
            print_ps("blended (translucent content):", rest / blend.blended);
        }
        if (read.blended > 0) {
            uint64_t rest = read.ns * 1000 > read.copied * copy_ps ? read.ns * 1000 - read.copied * copy_ps : 0;
            print_ps("read and found opaque or see-through:", rest / read.blended);
        }
    }

    CHECK(os64_gui_window_destroy(s_win) == 0);
    os64_close((int32_t)s_glass);

    // A program dying with a plane placed: its pair goes back at burial,
    // which comes a kworker pass or two after its exit.
    uint64_t before = 0, after = 0;
    CHECK(read_pairs(&before));
    char *args[] = { "/tests/planetest", "--orphan", NULL };
    int64_t child = os64_spawn(args[0], args);
    int32_t code = -1;
    CHECK(child > 0);
    if (child > 0)
        CHECK(os64_wait(child, &code) == child && code == 0);
    bool back = false;
    for (int i = 0; i < 100 && !back; i++) {
        back = read_pairs(&after) && after == before;
        if (!back)
            os64_sleep(20);
    }
    CHECK(back);
    if (!back)
        os64_printf("planetest: pairs %lu before the orphan, %lu after\n", before, after);
    if (failures) {
        os64_printf("planetest: %d check(s) failed\n", failures);
        return PLANE_FAIL;
    }
    os64_printf("planetest: PASS\n");
    return PLANE_OK;
}
