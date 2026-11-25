// planetest — a window's plane from ring 3 (docs/design/pending/
// VIDEO_PLANE.md § 3): a picture thread publishing a moving pattern on a
// timer, under a see-through hole the window's thread places with its own
// publish.
//
//   - the picture thread creates the plane and is its creator: the window's
//     thread is refused its surface, its publish and its destroy;
//   - a place before any picture, damage short of the hole, and an overlay
//     outside its clip are refused, and change nothing;
//   - placed, the plane is drawn: the compositor's counts rise;
//   - placed, the plane cannot be destroyed, nor its window;
//   - the picture thread ends with its plane placed, and the plane goes
//     with it: the window can then be destroyed.
//
// And it prices the compositor's part (/sys/gui's plane line: what was
// drawn, and in how long), in three phases a second each: no overlay (every
// pixel a picture pixel copied), an overlay strip whose content pixels are
// translucent (each one blended), and the same strip with content pixels
// that are wholly opaque or wholly see-through (each one read and found to
// need no blend, which is what a scrim mostly is). The overlay budget
// (GUI_PLANE_OVERLAY_PIXELS) is a time bound priced from these numbers.
//
// A covered window is drawn nothing, so the fixture SKIPS when its window is
// covered rather than pricing nothing.

#include "os64/os64.h"
#include "os64/gui.h"
#include "os64/draw.h"
#include "os64/thread.h"

#define PLANE_OK   0x91A40000
#define PLANE_FAIL 0x91A40001
#define PLANE_SKIP 0x91A40002

#define CONTENT_W 640
#define CONTENT_H 400
#define PIC_W 480
#define PIC_H 270
#define HOLE_X 80
#define HOLE_Y 40
#define STRIP_H 48
#define PHASE_MS 1000

static int failures;
#define CHECK(c) do { if (!(c)) { os64_printf("planetest: FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

static int64_t s_win;
static volatile uint32_t s_plane;      // the picture thread's plane, once made
static volatile int s_pictured;        // its first picture is published
static volatile int s_stop;            // the window's thread says: end
static volatile uint64_t s_published;

static int64_t picture_thread(void *arg)
{
    (void)arg;
    int64_t name = os64_gui_plane_create(s_win, PIC_W, PIC_H);
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
    for (uint32_t frame = 0; !s_stop; frame++) {
        // Bars moving right, a row band moving down: a misplaced or stale
        // row shows on the glass.
        for (uint32_t y = 0; y < PIC_H; y++) {
            uint32_t *row = s.pixels + (size_t)y * s.pitch_px;
            uint32_t band = ((y + frame * 2) / 30) & 1 ? 0x00303030u : 0;
            for (uint32_t x = 0; x < PIC_W; x++)
                row[x] = ((((x + frame * 4) / 40) & 1) ? 0x00C04020u : 0x002060C0u) + band;
        }
        if (os64_gui_plane_publish(s_win, (uint32_t)name, PIC_W, PIC_H) == 0)
            s_published++;
        s_pictured = 1;
        os64_sleep(16);
    }
    // Ending with the plane placed: the kernel takes it out.
    return 0;
}

// The window's content: opaque everywhere but the hole, which is wholly
// see-through but for the strip along its foot, painted by `strip`.
enum { STRIP_NONE, STRIP_TRANSLUCENT, STRIP_OPAQUE_OR_CLEAR };

static void paint_content(os64_draw_ctx_t *ctx, int strip)
{
    os64_gui_surface_t *c = &ctx->surf;
    for (uint32_t y = 0; y < c->height; y++) {
        uint32_t *row = c->pixels + (size_t)y * c->pitch_px;
        for (uint32_t x = 0; x < c->width; x++) {
            bool in_hole = x >= HOLE_X && x < HOLE_X + PIC_W && y >= HOLE_Y && y < HOLE_Y + PIC_H;
            bool in_strip = in_hole && y >= HOLE_Y + PIC_H - STRIP_H;
            uint32_t px = 0xFF283038u;
            if (in_strip && strip == STRIP_TRANSLUCENT)
                px = 0x80202020u;   // half covering, premultiplied
            else if (in_strip && strip == STRIP_OPAQUE_OR_CLEAR)
                px = ((x / 8 + y / 8) & 1) ? 0xFFE0E0E0u : 0x00000000u;
            else if (in_hole)
                px = 0;
            row[x] = px;
        }
    }
}

static os64_gui_plane_edit_t place(uint32_t plane, int strip)
{
    os64_gui_plane_edit_t e = {0};
    e.plane = plane;
    e.origin = (os64_gui_rect_t){HOLE_X, HOLE_Y, PIC_W, PIC_H};
    e.clip = e.origin;
    if (strip != STRIP_NONE) {
        e.noverlays = 1;
        e.overlay[0] = (os64_gui_rect_t){HOLE_X, HOLE_Y + PIC_H - STRIP_H, PIC_W, STRIP_H};
    }
    return e;
}

// /sys/gui's line for this window's plane: what was composed, and the time.
typedef struct {
    uint64_t copied, blended, opaque, ns;
} composed_t;

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

static bool read_composed(uint32_t plane, composed_t *out)
{
    int64_t h = os64_open("/sys/gui", "r");
    if (h < 0)
        return false;
    int64_t n = 0, got;
    while (n < (int64_t)sizeof(s_text) - 1 && (got = os64_read((int32_t)h, s_text + n, sizeof(s_text) - 1 - (size_t)n)) > 0)
        n += got;
    os64_close((int32_t)h);
    s_text[n] = 0;
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

// One phase: placed with `strip`, a second of pictures, the counts' rise.
static bool phase(os64_draw_ctx_t *ctx, uint32_t plane, int strip, composed_t *rise)
{
    paint_content(ctx, strip);
    os64_gui_plane_edit_t e = place(plane, strip);
    int64_t rc = os64_gui_window_publish_edits(s_win, NULL, &e, 1);
    CHECK(rc == 0);
    composed_t a, b;
    if (rc != 0 || !read_composed(plane, &a))
        return false;
    os64_sleep(PHASE_MS);
    if (!read_composed(plane, &b))
        return false;
    *rise = (composed_t){b.copied - a.copied, b.blended - a.blended, b.opaque - a.opaque, b.ns - a.ns};
    return true;
}

// Picoseconds a pixel, printed as nanoseconds with two places.
static void print_ps(const char *what, uint64_t ps)
{
    os64_printf("planetest: %s %lu.%02lu ns a pixel\n", what, ps / 1000, (ps % 1000) / 10);
}

int main(void)
{
    s_win = os64_gui_window_create("planetest", 60, 60, CONTENT_W, CONTENT_H, OS64_GUI_CREATE_CONTENT_SIZE);
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
    CHECK(os64_gui_plane_publish(s_win, plane, PIC_W, PIC_H) == OS64_GUI_ERR_NOT_CREATOR);
    CHECK(os64_gui_plane_destroy(s_win, plane) == OS64_GUI_ERR_NOT_CREATOR);

    // Refused commits change nothing.
    os64_gui_rect_t short_damage = {0, 0, 40, 40};
    os64_gui_plane_edit_t e = place(plane, STRIP_NONE);
    CHECK(os64_gui_window_publish_edits(s_win, &short_damage, &e, 1) == OS64_GUI_ERR_PLANE_DAMAGE);
    os64_gui_plane_edit_t out = place(plane, STRIP_TRANSLUCENT);
    out.overlay[0].x = 0;
    CHECK(os64_gui_window_publish_edits(s_win, NULL, &out, 1) == OS64_GUI_ERR_PLANE_OVERLAP);
    os64_gui_plane_edit_t bogus = place(plane + 1, STRIP_NONE);
    CHECK(os64_gui_window_publish_edits(s_win, NULL, &bogus, 1) == OS64_GUI_ERR_NO_PLANE);

    composed_t copy = {0}, blend = {0}, read = {0};
    bool priced = phase(&ctx, plane, STRIP_NONE, &copy);
    CHECK(priced && copy.copied > 0);
    // While placed: neither the window nor the plane may go.
    CHECK(os64_gui_window_destroy(s_win) == OS64_GUI_ERR_BUSY);
    priced = priced && phase(&ctx, plane, STRIP_TRANSLUCENT, &blend);
    CHECK(priced && blend.blended > 0);
    priced = priced && phase(&ctx, plane, STRIP_OPAQUE_OR_CLEAR, &read);
    CHECK(priced && read.blended > 0);

    // The picture thread ends with its plane placed: the plane goes with it.
    s_stop = 1;
    os64_thread_join((int32_t)th, NULL);
    CHECK(os64_gui_plane_publish(s_win, plane, PIC_W, PIC_H) == OS64_GUI_ERR_NO_PLANE);

    if (priced && copy.copied > 0) {
        uint64_t copy_ps = copy.ns * 1000 / copy.copied;
        os64_printf("planetest: %lu pictures published; composed %lu copied, %lu blended, %lu opaque\n",
                    s_published, copy.copied + blend.copied + read.copied, blend.blended + read.blended,
                    copy.opaque + blend.opaque + read.opaque);
        print_ps("copied (no overlay):", copy_ps);
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
    if (failures) {
        os64_printf("planetest: %d check(s) failed\n", failures);
        return PLANE_FAIL;
    }
    os64_printf("planetest: PASS\n");
    return PLANE_OK;
}
