// gfxprobe.c — ring 3 knocks on the GUI's new door.
//
// The first userland program ever to speak syscalls 16-21, and therefore
// the smoke test for GRAPHICS.md migration step 2: every row gets called
// once from the far side of the ring boundary, including the answers only
// ring 3 can provoke — a title copied across CR3s, an out-struct copied
// back, and GUI_ERR_NOT_OWNER for a window some kernel daemon owns.
//
// Two deliberate choices:
//
//   THE PROBE EXITS WITH ITS WINDOW STILL OPEN. That is not sloppiness —
//   it is the test. Step 1's exit sweep (gui_task_destroy_windows) has
//   only ever run as a no-op until now; this is its first real customer,
//   and the acceptance is visual (the window vanishes at exit) plus the
//   DEBUG_GUI line naming the sweep. Cleaning up after ourselves here
//   would UN-test the machinery that cleans up after everyone.
//
//   THE SURFACE ASSERTIONS ARE LOOSE. Content is inset from the frame by
//   chrome whose exact metrics will belong to the THEME TABLE one day —
//   a probe that hard-codes today's titlebar height would start failing
//   the moment customization works, which is backwards.
//
// Until the surface pivot (step 3), get_surface reports true geometry and
// a NULL pixel pointer — "you cannot draw yet", said truthfully. This
// probe asserts that NULL: the day the pivot lands, this assertion fails,
// and updating it to assert a usable task VA is part of that step's work.
//
// Exit codes name the failing step, per the house fixture convention.
// On a boot without GUI, every check downgrades to a SKIP (exit 0) — the
// suite doctrine: "I cannot run here" is not "I failed".

#include "os64/os64.h"
#include "os64/syscall.h"
#include "os64/syscall_numbers.h"

#define PROBE_OK            0
#define PROBE_SCREEN_INFO   2
#define PROBE_CREATE        3
#define PROBE_GET_SURFACE   4
#define PROBE_SURFACE_SHAPE 5
#define PROBE_PUBLISH       6
#define PROBE_POLL          7
#define PROBE_OWNERSHIP     8   // the fence FAILED — worth a loud noise

// The GUI error values (gui_client.h). Mirrored here rather than included:
// the kernel header drags in kernel types, and the ABI-published gui.h that
// will own these numbers arrives with libos64gfx (migration step 4).
#define GUI_ERR_NOT_RUNNING (-4)
#define GUI_ERR_NOT_OWNER   (-5)

// Layout mirror of the kernel's surface_t (gui_types.h): pointer + three
// uint32s. The rows froze this shape when they went live; libos64gfx's ABI
// header formalizes it at step 4.
typedef struct {
    uint64_t pixels;
    uint32_t width;
    uint32_t height;
    uint32_t pitch_px;
} probe_surface_t;

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    // 21: screen_info — the cheapest question, and the GUI-present gate.
    uint32_t sw = 0, sh = 0;
    int64_t rc = (int64_t)os64_syscall2(SYSCALL_GUI_SCREEN_INFO,
                                        (uint64_t)&sw, (uint64_t)&sh);
    if (rc == GUI_ERR_NOT_RUNNING)
    {
        os64_printf("gfxprobe: SKIP — GUI not running on this boot\n");
        return PROBE_OK;
    }
    if (rc != 0 || sw == 0 || sh == 0)
    {
        os64_hprintf(OS64_STDERR, "gfxprobe: screen_info rc=%ld %ux%u\n",
                     (long)rc, sw, sh);
        return PROBE_SCREEN_INFO;
    }
    os64_printf("gfxprobe: screen %ux%u\n", sw, sh);

    // 16: create — the title crosses the ring boundary by copy.
    int64_t win = (int64_t)os64_syscall6(SYSCALL_GUI_WINDOW_CREATE,
                                         (uint64_t)"probe",
                                         700, 80, 220, 140, 0);
    if (win <= 0)
    {
        os64_hprintf(OS64_STDERR, "gfxprobe: create rc=%ld\n", (long)win);
        return PROBE_CREATE;
    }
    os64_printf("gfxprobe: window handle %ld\n", (long)win);

    // 18: get_surface — geometry out, and (pre-pivot) a NULL canvas.
    probe_surface_t surf = {0xDEADBEEF, 0, 0, 0};
    rc = (int64_t)os64_syscall2(SYSCALL_GUI_WINDOW_GET_SURFACE,
                                (uint64_t)win, (uint64_t)&surf);
    if (rc != 0)
    {
        os64_hprintf(OS64_STDERR, "gfxprobe: get_surface rc=%ld\n", (long)rc);
        return PROBE_GET_SURFACE;
    }
    // Loose on purpose (chrome metrics are the future theme table's), but
    // the content must be smaller than the frame, non-empty, and pre-pivot
    // the pixel pointer must be NULL — a kernel VA here is a leak.
    if (surf.pixels != 0 || surf.width == 0 || surf.width > 220 ||
        surf.height == 0 || surf.height > 140 || surf.pitch_px < surf.width)
    {
        os64_hprintf(OS64_STDERR,
                     "gfxprobe: surface shape wrong: pixels=0x%lx %ux%u pitch %u\n",
                     (unsigned long)surf.pixels, surf.width, surf.height,
                     surf.pitch_px);
        return PROBE_SURFACE_SHAPE;
    }
    os64_printf("gfxprobe: content %ux%u, canvas withheld until the pivot — correct\n",
                surf.width, surf.height);

    // 19: publish — NULL damage (whole content), then a sub-rect. The
    // canvas is zeroed at create, so this paints an honest black body.
    rc = (int64_t)os64_syscall2(SYSCALL_GUI_WINDOW_PUBLISH, (uint64_t)win, 0);
    if (rc == 0)
    {
        int32_t rect[4] = {2, 2, 10, 10};   // rect_t: x, y, w, h
        rc = (int64_t)os64_syscall2(SYSCALL_GUI_WINDOW_PUBLISH,
                                    (uint64_t)win, (uint64_t)rect);
    }
    if (rc != 0)
    {
        os64_hprintf(OS64_STDERR, "gfxprobe: publish rc=%ld\n", (long)rc);
        return PROBE_PUBLISH;
    }

    // 20: poll — 0 (empty) and 1 (event copied out) are both legal answers;
    // negatives are not. 64 bytes comfortably holds an input_event_t.
    uint8_t event[64];
    rc = (int64_t)os64_syscall2(SYSCALL_GUI_EVENT_POLL,
                                (uint64_t)win, (uint64_t)event);
    if (rc < 0)
    {
        os64_hprintf(OS64_STDERR, "gfxprobe: poll rc=%ld\n", (long)rc);
        return PROBE_POLL;
    }

    // THE FENCE. Handle 1 belongs to whichever kernel daemon created its
    // window first (the console, in every boot so far) — destroying it must
    // answer NOT_OWNER and nothing else. If this "succeeds", ring 3 just
    // killed a kernel window and the fence is down: the loudest exit code
    // this probe has.
    if (win != 1)
    {
        rc = (int64_t)os64_syscall2(SYSCALL_GUI_WINDOW_DESTROY, 1, 0);
        if (rc != GUI_ERR_NOT_OWNER)
        {
            os64_hprintf(OS64_STDERR,
                         "gfxprobe: OWNERSHIP FENCE DOWN — destroy(1) rc=%ld (wanted %d)\n",
                         (long)rc, GUI_ERR_NOT_OWNER);
            return PROBE_OWNERSHIP;
        }
        os64_printf("gfxprobe: another task's window refused us — fence holds\n");
    }

    // And out — WITHOUT destroying our window. The exit sweep's first real
    // customer: watch it vanish, and watch DEBUG_GUI name the reaping.
    os64_printf("gfxprobe: all rows answered; exiting with the window open (the sweep's turn)\n");
    return PROBE_OK;
}
