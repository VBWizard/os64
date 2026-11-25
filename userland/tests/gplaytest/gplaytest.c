// gplaytest — /bin/gplay end to end on the clips that ship, each run with
// --check, which makes gplay itself judge its counts at the end (it played
// to the end, showed pictures, and every frame of sound it decoded was
// written or let go for a reason it counted) and exit 3 when one is wrong.
//
//   1. a file with both (bbb-480p-3s-av.mp4) plays to its end;
//   2. the same with the window covered for the whole clip (--cover): no
//      frame answer comes, the pictures are retired by the clock, and the
//      sound still plays to its end;
//   3. a file with no sound (bbb-480p-3s.mp4) plays on the monotonic clock;
//   4. a file with no picture (audio-tone.mp3) is refused with exit 2;
//   5. the sync clip with a script of keys: pause and resume, a seek each
//      way, mute and unmute, a resize, the readout, the sync dial both ways;
//   6. the sync clip with /dev/audio held by `play` at the start: gplay is
//      refused, plays on the monotonic clock, tries again and gets it
//      (--expect-refusal);
//   7. the sync clip covered and muted for its ten seconds: nobody asks
//      for a frame and nothing is heard, yet the player retires every
//      picture by the clock and drains its sound, and the end comes;
//   8. the file with no sound covered: the end comes on the clock alone;
//   9. a file that opens but is not media (a page) is refused with
//      exit 2: the player fails its probe and stays gplay's until closed;
//  10. the file with both again with --no-plane: the window paints every
//      picture, as with a plane refused.
//
// Every --check run but step 10 also has gplay judge that its plane was on
// to the end (VIDEO_PLANE.md § 6), so steps 1 to 8 prove the plane's path
// and step 10 the window's.
//
// Steps 2, 7 and 8 are the player's promise that a caller who never asks
// for a frame does not stop it (os64/media_player.h § A picture nobody asks
// for): with no frame call there is no other thread of the caller's to
// keep the queues moving.
//
// Timing is no result here: under QEMU's emulation the pictures fall behind
// and are dropped, skipped and resynced, which is the late policy working.
//
// Exit codes: 0x6F1A0000 success, 0x6F1A00NN the step that failed;
// 0x6F1A00FE skipped, because something can be heard (sound_guard.h) or
// the boot has no desktop (the line printed says which).

#include "../audiotest/sound_guard.h"
#include "os64/os64.h"
#include "os64/thread.h"

#define OK 0x6F1A0000
#define STEP(n) (0x6F1A0000 | (n))
#define AV "/tests/pages/bbb-480p-3s-av.mp4"
#define PICTURE_ONLY "/tests/pages/bbb-480p-3s.mp4"
#define SOUND_ONLY "/tests/pages/audio-tone.mp3"
#define SYNC "/tests/pages/sync-720p60.mp4"
#define PATIENCE_TURNS 2400               // 50 ms each: two minutes

typedef struct { int64_t kid; int32_t code; volatile bool gone; } waiter_t;

static int64_t collect(void *arg)
{
    waiter_t *w = arg;
    w->code = -1;
    os64_wait(w->kid, &w->code);
    __atomic_store_n(&w->gone, true, __ATOMIC_RELEASE);
    return 0;
}

// gplay with `argv`; its exit code, or -1 when it could not be run or had
// not ended within two minutes (it is killed then).
static int run(char *const argv[])
{
    waiter_t w = {0, -1, false};
    w.kid = os64_spawn_redirected("/bin/gplay", argv, -1, -1, -1, 0);
    if (w.kid < 0)
        return -1;
    int64_t thread = os64_thread(collect, &w);
    if (thread < 0)
        return -1;
    for (int turns = 0; turns < PATIENCE_TURNS; turns++) {
        if (__atomic_load_n(&w.gone, __ATOMIC_ACQUIRE)) {
            os64_thread_join((int32_t)thread, 0);
            return w.code;
        }
        os64_sleep(50);
    }
    char ctl[48];
    os64_snprintf(ctl, sizeof(ctl), "/proc/%ld/ctl", (long)w.kid);
    int64_t h = os64_open(ctl, "w");
    if (h >= 0) {
        os64_write((int32_t)h, "kill", 4);
        os64_close((int32_t)h);
    }
    os64_thread_join((int32_t)thread, 0);
    return -1;
}

static int expect(int step, char *const argv[], int want)
{
    int got = run(argv);
    if (got == want) {
        os64_printf("gplaytest: step %d ok\n", step);
        return 0;
    }
    os64_printf("gplaytest: FAIL step %d: gplay exited %d, want %d\n", step, got, want);
    return (int)STEP(step);
}

static int steps(void)
{
    int r;
    if ((r = expect(1, (char *[]){"/bin/gplay", "--check", AV, 0}, 0)) != 0)
        return r;
    if ((r = expect(2, (char *[]){"/bin/gplay", "--check", "--cover", AV, 0}, 0)) != 0)
        return r;
    if ((r = expect(3, (char *[]){"/bin/gplay", "--check", PICTURE_ONLY, 0}, 0)) != 0)
        return r;
    if ((r = expect(4, (char *[]){"/bin/gplay", "--check", SOUND_ONLY, 0}, 2)) != 0)
        return r;
    char *script = "300:pause,900:pause,1500:forward,2200:back,2700:mute,3300:mute,3800:resize=480x270,"
                   "4300:readout,4600:later,4700:earlier,5200:pause,5500:resize=640x360,5900:pause";
    if ((r = expect(5, (char *[]){"/bin/gplay", "--check", "--script", script, SYNC, 0}, 0)) != 0)
        return r;
    // The device busy at the start: play holds it for its two seconds, and
    // gplay plays on the monotonic clock, tries again, and gets it.
    int64_t quiet = os64_open("/dev/null", "w");
    int64_t holder = os64_spawn_redirected("/bin/play", (char *[]){"/bin/play", SOUND_ONLY, 0}, -1,
                                           (int32_t)quiet, (int32_t)quiet, 0);
    if (quiet >= 0)
        os64_close((int32_t)quiet);
    if (holder < 0) {
        os64_printf("gplaytest: FAIL step 6: play would not start\n");
        return (int)STEP(6);
    }
    os64_sleep(300);   // play has the device
    r = expect(6, (char *[]){"/bin/gplay", "--check", "--expect-refusal", SYNC, 0}, 0);
    int32_t code = 0;
    os64_wait(holder, &code);
    if (r != 0)
        return r;
    if ((r = expect(7, (char *[]){"/bin/gplay", "--check", "--cover", "--script", "100:mute", SYNC, 0}, 0)) != 0)
        return r;
    if ((r = expect(8, (char *[]){"/bin/gplay", "--check", "--cover", PICTURE_ONLY, 0}, 0)) != 0)
        return r;
    if ((r = expect(9, (char *[]){"/bin/gplay", "--check", "/tests/pages/audio.html", 0}, 2)) != 0)
        return r;
    return expect(10, (char *[]){"/bin/gplay", "--check", "--no-plane", AV, 0}, 0);
}

int main(void)
{
    char sink[32];
    if (sound_guard_audible(sink, sizeof(sink))) {
        os64_printf("gplaytest: SKIP: %s can be heard\n", sink);
        return (int)0x6F1A00FE;
    }
    uint32_t sw = 0, sh = 0;
    if (os64_gui_screen_info(&sw, &sh) < 0 || sw == 0) {
        os64_printf("gplaytest: SKIP: no desktop on this boot\n");
        return (int)0x6F1A00FE;
    }
    int volume = sound_guard_volume_get();
    int r = steps();
    if (volume >= 0)
        sound_guard_volume_set(volume);   // as found, whatever a step did to it
    if (r == 0)
        os64_printf("gplaytest: PASS\n");
    return r == 0 ? OK : r;
}
