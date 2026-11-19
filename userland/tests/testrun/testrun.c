// testrun — the ring-3 test runner.
//
// WHY THIS EXISTS (Chris's call, 2026-08-09, after a day lost to the
// alternative). Sixteen of the post-boot tests were kernel functions whose
// entire body was: spawn a ring-3 fixture, then POLL the child's task_t for
// `exited` and `retVal`. The fixture did all the real work; the kernel was a
// middleman peeking at a struct.
//
// That middleman had no synchronization contract — only a timing one, spelled
// out in test_spawn's own comment: the poller must finish reading the struct
// before kworker frees it, with the burial latency as the entire safety
// margin. Since every allocation in os64 is zeroed, a struct freed and reused
// mid-poll reads `exited == 0`, which is indistinguishable from "the child
// never finished". Result: `elf_loader`, `task_args` and `dynamic_linking`
// failing intermittently under SCHED=periodic for weeks, blamed on the
// scheduler, while the kernel was innocent. Measured on 2026-08-09: 8 of 10
// boots failed at least one of them.
//
// The cure is to stop reaching inside. os64_wait() IS the contract — a corpse
// cannot be buried out from under a waiter, because COLLECTING it is what
// licenses the burial (task.h, the death certificate). So the test becomes an
// ordinary program that spawns children and waits for them, the same way husk
// does, exercising the same syscalls a real user does. A struct-poking test
// can pass while the syscall path is broken; this one cannot.
//
// That is also the older tradition. Unix has always tested kernels from
// userland — the syscall boundary IS the contract, so that is where you push
// on it. Linux's kselftest is a pile of ordinary C programs; in-kernel unit
// testing arrived decades later and stayed where it belongs, on pure
// computation. os64's 24 pre-boot tests (kmalloc, dlist, arena, VMA, CoW) run
// before userland exists and keep their kernel seat forever.
//
// WHAT STAYED BEHIND, deliberately: two of the migrated tests asserted on
// kernel-internal state that no program can see — elf_loader counts demand
// page faults (kPageFaultCount), and dynamic_linking checks that two tasks
// resolve to the SAME shared_object_t through the registry. Those assertions
// remain in the kernel as slim tests; only the "does it run and exit
// correctly" half moved here. Nothing lost coverage in the move, and neither
// remnant polls a task struct.

#include "os64/os64.h"

// A fixture's verdict rides home in its EXIT CODE, and every one of these
// magic numbers is a word spelled in hex — 0x0F11E60D is "FILE GOOD",
// 0x0D12600D is "DIR GOOD", 0xE1F0CA11 is "ELF OK CALL". They were chosen so
// that a wrong answer on the wire is unmistakably a DIFFERENT fixture's
// badge rather than a plausible-looking integer.
typedef struct {
    const char *path;        // the fixture, as it lives on the image
    char *const *argv;       // NULL = just the path, no arguments
    uint32_t     expect;     // the pass code
    uint32_t     skipcode;   // this code means SKIP, not fail (0 = none)
    const char  *why;        // what a failure would mean, for the log line
} fixture_t;

static char *const argv_arg_echo[] = { "/tests/arg_echo", "hello", "world", NULL };

// H.264 decoding is bit-exact, so the clip's first 30 frames give one CRC in
// every mode, on every machine, and on the host where it was computed
// (zlib's CRC-32 over the frames' visible planes; avbench.c says exactly
// how). 30 frames keep the rows to a few seconds under QEMU's emulation.
// The default mode decodes with whatever routines FFmpeg chose for this
// CPU; -c with its C ones. The tiny clip is decoded whole.
static char *const argv_avbench_auto[] = { "/tests/avbench", "-n", "30", "-k", "96e43679",
                                           "/tests/pages/bbb-480p-3s.mp4", NULL };
static char *const argv_avbench_c[]    = { "/tests/avbench", "-c", "-n", "30", "-k", "96e43679",
                                           "/tests/pages/bbb-480p-3s.mp4", NULL };
static char *const argv_avbench_tiny[] = { "/tests/avbench", "-k", "fa7a3f20",
                                           "/tests/pages/video-tiny.mp4", NULL };
// The same 30 frames converted to a window's pixels at 640x360 by the
// scaler's bit-exact bilinear, whose pixels do not depend on the routines
// FFmpeg chose: one CRC with the CPU's routines and with the C ones. It is
// the port's own measurement (the host's FFmpeg is another release, whose
// scaler differs), taken in QEMU with every tier agreeing.
static char *const argv_avbench_x_auto[] = { "/tests/avbench", "-n", "30", "-k", "96e43679", "-x", "640x360", "-e",
                                             "-K", "a434a9b9", "/tests/pages/bbb-480p-3s.mp4", NULL };
static char *const argv_avbench_x_c[]    = { "/tests/avbench", "-c", "-n", "30", "-k", "96e43679", "-x", "640x360",
                                             "-e", "-K", "a434a9b9", "/tests/pages/bbb-480p-3s.mp4", NULL };
// Enlarging a 16-wide picture to 64: the case where libswscale's fast
// bilinear would write code into the heap and run it (avbench.c, -x),
// which os64's no-execute data memory ends with exit 139.
static char *const argv_avbench_x_enlarge[] = { "/tests/avbench", "-k", "fa7a3f20", "-x", "64x64",
                                                "/tests/pages/video-tiny.mp4", NULL };

// The crime fixtures that PASS BY DYING: each commits a deliberate offense
// and the pass code is the badge the enforcement kills with. A crime that
// failed to kill would report the program's own exit code instead — which is
// exactly the failure worth catching, since a tripwire nobody tests is a
// tripwire nobody knows is disconnected.
static char *const argv_malloc_threads[]    = { "/tests/malloctest", "threads", "4", NULL };
static char *const argv_malloc_doublefree[] = { "/tests/malloctest", "doublefree", NULL };
static char *const argv_html_pinned[]       = { "/tests/htmltest", "pinned", NULL };
static char *const argv_malloc_stomp[]      = { "/tests/malloctest", "stomp", NULL };

// The W^X pair (2026-08-16): both die by the segfault kill, 139. A survivor
// exits 0x0BAD and the mismatch names the disconnected tripwire.
static char *const argv_nx_stack[] = { "/tests/nx_test", "stack", NULL };
static char *const argv_nx_text[]  = { "/tests/nx_test", "text",  NULL };
static char *const argv_fpfault_xm[] = { "/tests/fpfault", "xm", NULL };
static char *const argv_fpfault_mf[] = { "/tests/fpfault", "mf", NULL };
static char *const argv_fpfault_de[] = { "/tests/fpfault", "de", NULL };
static char *const argv_fpfault_ud[] = { "/tests/fpfault", "ud", NULL };

static const fixture_t kFixtures[] = {
    { "/tests/syscall_smoke",   NULL, 0x0005E00D,  0,          "the syscall floor: write/exit from ring 3" },
    { "/tests/exit_by_return",  NULL, 0x2E7BEA57,  0,          "returning from _start reaches retVal" },
    { "/tests/arg_echo",        argv_arg_echo, 0x0A11600D, 0,  "argc/argv/env delivered at the ABI addresses" },
    { "/tests/file_io",         NULL, 0x0F11E60D,  0,          "open/read/write/seek/close on a real file" },
    { "/tests/redirect_io",     NULL, 0x2ED1600D,  0,          "spawn-time handle redirection" },
    { "/tests/dir_list",        NULL, 0x0D12600D,  0,          "opendir/readdir/close" },
    { "/tests/map_unmap",       NULL, 0x03A9600D,  0,          "map/unmap — malloc's wall" },
    { "/tests/cwd_test",        NULL, 0x0C3D600D,  0,          "getcwd/chdir" },
    // stat_test reports 0x57A70007 when the boot's mount policy
    // (/etc/mounts.conf) mounts no disk beyond root — nothing to route a
    // disk stat across. A fact about the BOOT, not a failure of stat; the
    // routing half still ran against /sys. Same treatment as synctest.
    { "/tests/stat_test",       NULL, 0x57A7600D,  0x57A70007, "stat is readdir for exactly one name" },
    { "/tests/sleep_test",      NULL, 0x51EE600D,  0,          "sleep parks at least as long as asked" },
    { "/tests/memory_test",     NULL, 0xF3EE600D,  0,          "the memory syscall's snapshot" },
    { "/tests/threadtest",      NULL, 0x1B2EAD00,  0,          "threads: create, argument, join, shared address space" },
    { "/tests/worktest",        NULL, 0x600C0000,  0x600C0002, "Worker pool: ownership, admission, cancellation, shutdown" },
    { "/tests/doorbelltest",    NULL, 0x600B0000,  0x600B0002, "GUI doorbell: owner, coalescing, overflow, worker wake, libui" },
    { "/tests/guiwake",         NULL, 0x60E00000,  0x60E00002, "GUI event wait: a ring landing mid-park wakes the waiter, not its backstop" },
    { "/tests/belltest",        NULL, 0xBE110000,  0,          "the bell: a ring is kept, one waiter, close and a handled signal end a wait, and a ring landing mid-park wakes the waiter" },
    { "/tests/audiotest",       NULL, 0xA0D10000,  0xA0D100FE,        "sound as a file: one owner, the ramp at the tap sample for sample, patience, pause, flush, a signal, close, volume, two generations, a shared handle" },
    { "/tests/mediakeystest",   NULL, 0x3E4B0000,  0x3E4B00FE,        "the media keys: the rule (playing, most recently active, the toggle), a full ring passed over, BUSY, a press, a signal and a close end a parked read, the kernel's answer, a killed session, the wall" },
    { "/tests/resampletest",    NULL, 0x5A3E0000,  0,          "libsound's resampler: a sine at 44.1 kHz comes out the same sine at 48, and back, and mono" },
    { "/tests/playtest",        NULL, 0x91A10000,  0x91A100FE,        "/bin/play end to end at the tap: a 44.1 kHz mono WAV is the same sine at 48, a 48 kHz one arrives sample for sample, bad files are refused, an MP3 is heard whole" },
    // gplaytest reports 0x6F1A00FE on a boot with no desktop, or with a
    // sink that can be heard: the line it prints says which.
    { "/tests/gplaytest",       NULL, 0x6F1A0000,  0x6F1A00FE,        "/bin/gplay end to end: a video plays to its end with every frame of sound accounted for, covered for the whole clip too, a silent file, a soundless one refused, a script of keys (pause, seeks, mute, resizes, the readout, the sync dial)" },
    { "/tests/avtest",          NULL, 0xA7E50000,  0,          "libav.so (FFmpeg): an MP3 in memory probes, its tag reads, it decodes to one second of the same sine and seeks; a WAV decodes sample for sample; noise is refused; an MP4 with a picture yields its AAC sound" },
    { "/tests/mediatest",       NULL, 0x3ED10000,  0,          "libmedia.so: MP3 and WAV to 48 kHz stereo on the media's timeline, exact seeks, a 5.1 fold, an unseekable source, noise refused, two threads at once; its type answers; an MP4's sound" },
    { "/tests/avbench",         argv_avbench_auto, 0xAB0C0000, 0, "libav.so's H.264 decoder: 30 frames of Big Buck Bunny give the host's CRC with the routines FFmpeg chose for this CPU" },
    { "/tests/avbench",         argv_avbench_c,    0xAB0C0000, 0, "libav.so's H.264 decoder: the same 30 frames give the same CRC with FFmpeg's C routines" },
    { "/tests/avbench",         argv_avbench_tiny, 0xAB0C0000, 0, "libav.so's H.264 decoder: the 16x16 clip decodes whole to the host's CRC" },
    { "/tests/avbench",         argv_avbench_x_auto, 0xAB0C0000, 0, "libav.so's scaler: the 30 frames, converted bit-exact to 640x360 window pixels with the routines FFmpeg chose, give the port's CRC" },
    { "/tests/avbench",         argv_avbench_x_c,    0xAB0C0000, 0, "libav.so's scaler: the same conversion with FFmpeg's C routines gives the same CRC" },
    { "/tests/avbench",         argv_avbench_x_enlarge, 0xAB0C0000, 0, "libav.so's scaler: enlarging, the fast scaler runs no code written into data memory (it is bilinear there)" },
    { "/tests/titletest",       NULL, 0x711E0000,  0x711E0002, "GUI rename: the owner retitles and sets the encoding; create's title refusals, another task's NOT_OWNER" },
    { "/tests/frametest",       NULL, 0xF4A30000,  0xF4A30002, "the display's frame: a request is answered, numbers rise, twice is once, if-drawn waits for a publish, take leaves other events, an unread answer is one, an asker dying unanswered breaks nothing" },
    { "/tests/pintest",         NULL, 0x91A70000,  0,          "a handle closed by a sibling thread under a parked read: pipe, pty master, thread handle" },
    { "/tests/pincputest",      NULL, 0xC02E0000,  0,          "PINCPU=<n> pins a program and its threads to CPU n; PINCORE still works; bad values leave it unpinned" },
    { "/tests/streamseat",      NULL, 0x57EA0000,  0,          "STREAM EOF refuses another seat; failed load and GRID reuse" },
    { "/tests/argsize",         NULL, 0xA2650000,  0,          "128 KiB spawn arguments arrive whole; too long, too many and too much are refused by name" },
    { "/tests/pipeexit",        NULL, 0x91E00000,  0,          "32 default-SIGPIPE exits preserve status 141" },
    { "/tests/spawnreap",       NULL, 0x5A9E0000,  0,          "128 short-lived children with concurrent sibling reap" },
    { "/tests/exitcode",        NULL, 0xE817C000,  0,          "a program ending with live threads reports its own exit code; a killed one reports 137" },
    { "/tests/malloctest",      NULL, 0x0A110C00,  0,          "the heap: split, coalesce, recycle, give-back, /proc/<pid>/heap" },
    { "/tests/jssupporttest",   NULL, 0x4A535550,  0,          "libos64: live allocation capacity and unsigned-byte string/search semantics" },
    { "/tests/arenatest",       NULL, 0xA2E7A000,  0,          "arenas: alignment, growth, limits, zeroing, frame reuse and heap integrity" },
    { "/tests/malloctest",      argv_malloc_threads,    0x0A110C10, 0, "four threads, one heap: the lock, and cross-thread frees" },
    { "/tests/malloctest",      argv_malloc_doublefree, 0xF12EEBAD, 0, "a double free kills the program (it must)" },
    { "/tests/malloctest",      argv_malloc_stomp,      0xCA9A12ED, 0, "a stomped canary kills the program (it must)" },
    { "/tests/nx_test",         argv_nx_stack, 139, 0,         "executing the stack kills the program (NX works)" },
    { "/tests/nx_test",         argv_nx_text,  139, 0,         "writing to .text kills the program (W^X works)" },
    { "/tests/fputest",         NULL, 0xF0DE0000,  0,          "x87/SSE/AVX data AND control state survive preemption, migration, two threads on one CPU, a handler that wipes them on each delivery path, and a forged frame image" },
    { "/tests/mathtest",        NULL, 0x3A740000,  0,          "libmath.so matches the host build bit for bit, keeps the caller's FP control state, and holds across threads" },
    // PASS BY DYING: a CPU exception from ring 3 ends the program with
    // 200 + vector (user_exception_kill), never the machine.
    // #XM is the one QEMU's TCG cannot raise (it records SSE exceptions in
    // MXCSR and never traps), so the fixture answers 3 there and that is a
    // SKIP, not a failure; on hardware and under KVM it dies with 219.
    { "/tests/fpfault",         argv_fpfault_xm, 219, 3,       "an unmasked SSE exception (#XM) kills the program, not the OS" },
    { "/tests/fpfault",         argv_fpfault_mf, 216, 0,       "an unmasked x87 exception (#MF) kills the program, not the OS" },
    { "/tests/fpfault",         argv_fpfault_de, 200, 0,       "an integer divide by zero (#DE) kills the program, not the OS" },
    { "/tests/fpfault",         argv_fpfault_ud, 206, 0,       "an instruction whose state is off (#UD: AVX-512 with AVX on, AVX without) kills the program, not the OS" },
    // 2 = the CPU says AVX is off (`sse` mode, or no AVX at all): a SKIP.
    { "/tests/avxprobe",        NULL, 0, 2,                    "AVX, AVX2 and FMA give exact answers on CPU 0 and CPU 1, and a spawned program is born with zero upper halves" },
    // The guard below a thread's stack catches a runaway recursion
    // (-fstack-clash-protection keeps a big frame from leaping it), and the
    // kernel heads the death "Stack overflow". 139 is what testrun can see;
    // the headline is on the wire.
    { "/tests/stackprobe",      NULL, 139, 0,                  "a runaway recursion dies in the stack guard as a stack overflow, not the OS" },
    // And a program may ask for more: its PT_GNU_STACK size is its main
    // thread's (SCRIPT_STACK.md), and a thread it spawns keeps the default.
    { "/tests/stacktest",       NULL, 0x57AC600D, 0,           "a program's declared stack is its main thread's, deep as declared; a thread's is the default" },
    { "/tests/test_elf",        NULL, 0xE1F0CA11,  0,          "a demand-paged static ELF runs and exits" },
    { "/tests/dyn_consumer",    NULL, 0x00300031,  0,          "a dynamically-linked binary resolves and runs" },
    // synctest reports 0x05CC0001 when the boot has no writable /home. That
    // is a fact about the BOOT, not a failure of sync_all — the kernel
    // harness treated it as SKIP and so does this one.
    { "/tests/synctest",        NULL, 0x05CC0000,  0x05CC0001, "sync_all commits bytes AND the directory entry" },
    // conftest reports 0x0C0F0001 when a save could not be written at all —
    // which on a boot with no writable /home is a fact about the BOOT, not a
    // failure of the config library. Same treatment as synctest above, and
    // for the same reason: a suite that cries wolf gets ignored.
    { "/tests/conftest",        NULL, 0x0C0F0000,  0x0C0F0001, "config library: get/get_bool/write/set, merge and ext2 atomic publish" },
    { "/tests/tartest",         NULL, 0x7A120000,  0,          "ustar create/list/extract, pipelines, ext2 atomic publication and path safety" },
    { "/tests/gziptest",        NULL, 0x621A0000,  0,          "standalone libgzip and safe gzip/gunzip command publication" },
    { "/tests/pngtest",         NULL, 0x90640000,  0,          "standalone libpng: filtered RGBA decode, alpha, CRC and pixel cap" },
    { "/tests/pagetest",        NULL, 0x50670000,  0,          "libpage: numeric conversion, range grids, control state and submission" },
    { "/tests/htmltest",        NULL, 0x48640000,  0,          "standalone libhtml: document recovery, namespaces, templates, the verbs and heap lifetime" },
    { "/tests/htmltest",        argv_html_pinned, 0x48544D4C, 0, "freeing a pinned document kills the program (it must)" },
    { "/tests/jpegtest",        NULL, 0x90650000,  0,          "JPEG: baseline/progressive, orientation, image dispatch, file load and cleanup" },
    { "/tests/bearssltest",     NULL, 0xBEA20000,  0,          "BearSSL foundation vectors and explicit entropy boundary" },
    { "/tests/tlstrusttest",    NULL, 0x71570000,  0,          "TLS trust parsing, certificate gate and snapshot replacement" },
    { "/tests/tlsinputtest",    NULL, 0x71580000,  0,          "TLS creation with OS randomness, UTC time and owned trust" },
    { "/tests/tlslibtest",      NULL, 0x71590000,  0,          "public libtls: shared loading, OS inputs, trust lifetime and byte operations" },
    { "/tests/rngprobe",        NULL, 0,           3,          "CPU RNG availability and bounded sampling; not entropy certification" },
    { "/tests/randtest",        NULL, 0x5EED0000,  0,          "/dev/random: two distinct seeds, a page-capped read, a contribution, /sys/random seeded" },
    { "/tests/looptest",        NULL, 0x100BBAC0,  0,          "loopback: a door only this machine can reach, 3 MiB each way, localhost, and the refusals" },
    // glasstest reports 0x61A55001 on a boot with no desktop: there is no
    // screen to watch, a fact about the BOOT (synctest's treatment).
    { "/tests/glasstest",       NULL, 0x61A55000,  0x61A55001, "/dev/glass: the whole screen, banded, refused when too small, a painted window, a still screen, a close under a parked reader; and its hands: keys, chords, a click, repeat, release on close" },
    { "/tests/renametest",      NULL, 0x4E4D0000,  0,          "rename policies on ext2 and FAT, plus invalid flags and legacy ABI compatibility" },
    { "/tests/closetest",       NULL, 0xC10E0000,  0,          "a close that could not commit is heard in ring 3 (OS64_CLOSE_NOT_COMMITTED), and told apart from no-such-handle" },
    { "/tests/openxtest",        NULL, 0x0EAC7000,  0,          "atomic exclusive create on ext2 and FAT, including contention and full-handle refusal" },
    { "/tests/sigtest",         NULL, 0x05160000,  0,          "signal handlers: install, replace, restore, and the refusals" },
    { "/tests/winchtest",       NULL, 0x0A1D0000,  0,          "pty resize: the grid follows, the seats hear SIGWINCH, a blocked read and a blocked wait are interrupted" },
    { "/tests/rawtty",          NULL, 0x2A710000,  0,          "raw mode: cooked by default, 0x03/0x04 arrive as bytes on request, the foreground's wish, cooked again, reset at the asker's exit, strangers refused" },
    { "/tests/df_test",         NULL, 0x0DF00000,  0,          "the direction flag does not cross a ring boundary (syscall, and into a handler)" },
    { "/tests/regleak_test",    NULL, 0x02E60000,  0,          "a syscall returns no kernel state in its scratch registers" },
    // A kernel whose user-copy bound is wrong does not report a failure
    // here: it panics, on the copy.
    { "/tests/badptr",          NULL, 0xBADA0000,  0,          "a pointer ring 3 cannot own (non-canonical, kernel, NULL, crossing the ceiling) is refused by the copy helpers, a thread's entry and exit stub, and an ELF entry point" },
    // PASSES BY DYING, like the nx_test pair above: 141 is SIGPIPE's default
    // action (128 + 13), and reaching it proves the kernel still applies that
    // default when a handler is installed but CANNOT BE DELIVERED. Surviving
    // is the failure, and sigpipe_test says so with its own code rather than
    // hanging. Deliberately last — it is the only fixture that ends itself.
    { "/tests/sigpipe_test",    NULL, 141,         0,          "an undeliverable SIGPIPE still applies its default action" },
};
#define FIXTURE_COUNT (int32_t)(sizeof(kFixtures) / sizeof(kFixtures[0]))

static int32_t gPassed, gFailed, gSkipped;

// An optional NAME on the command line narrows the run to the fixtures
// whose path contains it — `testrun fputest` runs one program, `testrun
// fpfault` runs its four rows. The point is `watch -e testrun fputest`: a
// fixture passes with a distinctive badge (0xF0DE0000), which every
// exit-code-reading tool calls "nonzero", and the runner is the one thing
// that knows the badges. Substring, not glob: husk would expand a `*`
// against the cwd before testrun ever saw it.
static const char *gOnly;

static bool contains(const char *haystack, const char *needle)
{
    size_t n = os64_strlen(needle);
    for (; *haystack; haystack++)
    {
        size_t i = 0;
        while (i < n && haystack[i] == needle[i])
            i++;
        if (i == n)
            return true;
    }
    return false;
}

static bool selected(const char *path)
{
    return gOnly == NULL || contains(path, gOnly);
}

// One line per verdict, on the SERIAL WIRE. os64_serial_log rather than
// os64_debug_log on purpose: a harness boot usually runs with LOGD=, and a
// plain log line would land in a file inside the guest that nobody outside
// can read until shutdown. The beacon goes out the same door panic() uses.
static void report(const char *verdict, const char *name, const char *detail)
{
    char line[256];
    os64_snprintf(line, sizeof(line), "TESTRUN %s %s%s%s",
                  verdict, name, detail[0] ? " - " : "", detail);
    os64_serial_log(line);
    os64_printf("  %-5s %s\n", verdict, name);
}

// Spawn one fixture and WAIT for it. No struct is read, no flag is polled:
// the exit code arrives through os64_wait's out-parameter, which is the
// kernel's own promise and cannot race the undertaker.
static void run_fixture(const fixture_t *f)
{
    char *const solo[] = { (char *)f->path, NULL };
    char *const *argv = (f->argv != NULL) ? f->argv : solo;
    char detail[160];

    int64_t pid = os64_spawn(f->path, argv);
    if (pid < 0)
    {
        // A missing fixture is a BUILD failure, not a kernel failure, and it
        // must not masquerade as either — say which file is absent.
        os64_snprintf(detail, sizeof(detail), "cannot spawn (is %s on the image?)", f->path);
        report("FAIL", f->path, detail);
        gFailed++;
        return;
    }

    int32_t code = 0;
    if (os64_wait(pid, &code) < 0)
    {
        report("FAIL", f->path, "wait() failed");
        gFailed++;
        return;
    }

    uint32_t got = (uint32_t)code;
    if (f->skipcode != 0 && got == f->skipcode)
    {
        os64_snprintf(detail, sizeof(detail), "fixture reported 0x%08x", got);
        report("SKIP", f->path, detail);
        gSkipped++;
        return;
    }
    if (got != f->expect)
    {
        os64_snprintf(detail, sizeof(detail), "exit 0x%08x, expected 0x%08x (%s)",
                      got, f->expect, f->why);
        report("FAIL", f->path, detail);
        gFailed++;
        return;
    }
    report("PASS", f->path, "");
    gPassed++;
}

// The concurrency case the kernel suite called ring3_file_io_concurrent: TWO
// copies of the same fixture in flight at once, which is what caught the
// FatFs shared-sector-window bug in July. Both are spawned BEFORE either is
// waited on — waiting on the first before launching the second would test
// nothing but sequence.
static void run_concurrent_pair(void)
{
    const char *path = "/tests/file_io";
    char *const solo[] = { (char *)path, NULL };
    int64_t a = os64_spawn(path, solo);
    int64_t b = os64_spawn(path, solo);

    if (a < 0 || b < 0)
    {
        report("FAIL", "file_io x2 (concurrent)", "cannot spawn both");
        gFailed++;
        return;
    }

    int32_t ca = 0, cb = 0;
    bool oka = (os64_wait(a, &ca) >= 0) && ((uint32_t)ca == 0x0F11E60D);
    bool okb = (os64_wait(b, &cb) >= 0) && ((uint32_t)cb == 0x0F11E60D);

    if (oka && okb)
    {
        report("PASS", "file_io x2 (concurrent)", "");
        gPassed++;
    }
    else
    {
        char detail[160];
        os64_snprintf(detail, sizeof(detail), "exits 0x%08x / 0x%08x, expected 0x0F11E60D both",
                      (uint32_t)ca, (uint32_t)cb);
        report("FAIL", "file_io x2 (concurrent)", detail);
        gFailed++;
    }
}

int main(int argc, char **argv)
{
    bool with_net = false;
    const os64_optspec_t specs[] = {
        {'n', "net", false, "also run the fixtures that need a live network",
         .flag = &with_net}
    };
    os64_args_t args = {0};
    os64_args_init(&args, argc, argv, specs, 1);
    args.about = "Run the ring-3 test fixtures and report to the serial wire.";
    args.details = "Each fixture is spawned and waited on through the ordinary "
                   "syscalls, so no test can race the undertaker the way the "
                   "in-kernel versions did. NAME narrows the run to fixtures "
                   "whose path contains it, and the exit code is 0 only if "
                   "every one passed — so `watch -e testrun fputest` loops a "
                   "single fixture and stops at its first failure.";
    int32_t parsed = os64_args_parse(&args, "testrun [-n] [NAME]", &gOnly, 1);
    if (parsed == OS64_ARG_HELP) return 0;
    if (parsed < 0) return 2;

    os64_serial_log("TESTRUN: begin (ring-3 suite)");
    if (gOnly)
        os64_printf("testrun - fixtures matching '%s'\n", gOnly);
    else
        os64_printf("testrun - %d ring-3 fixtures\n", FIXTURE_COUNT);

    for (int32_t i = 0; i < FIXTURE_COUNT; i++)
        if (selected(kFixtures[i].path))
            run_fixture(&kFixtures[i]);

    // The concurrent pair is a property of the whole suite, not of a name.
    if (gOnly == NULL)
        run_concurrent_pair();

    // The network pair is opt-in: on a boot with no NIC (or no DHCP lease)
    // these fail for a reason that has nothing to do with the code under
    // test, and a suite that cries wolf gets ignored.
    if (with_net)
    {
        const fixture_t net[] = {
            { "/tests/dialtest",  NULL, 0x0D1A1600, 0, "net_dial round trip" },
            { "/tests/fetchtest", NULL, 0x0FE7C400, 0, "TCP fetch over the stack" },
        };
        for (int32_t i = 0; i < 2; i++)
            if (selected(net[i].path))
                run_fixture(&net[i]);
    }

    // A name that matches nothing is a typo, and a typo must not read as
    // "0 failed": say so, and exit the way a failure does.
    if (gOnly && gPassed + gFailed + gSkipped == 0)
    {
        os64_printf("testrun: no fixture matches '%s'\n", gOnly);
        return 2;
    }

    // THE LINE THE HARNESS GREPS. One format, on the wire, every run — the
    // in-kernel suite's equivalent ("BUILT-IN TESTS: N passed, M failed") is
    // what the A/B script keys on, and this is deliberately parallel to it.
    char summary[128];
    os64_snprintf(summary, sizeof(summary),
                  "TESTRUN: %d passed, %d failed, %d skipped",
                  gPassed, gFailed, gSkipped);
    os64_serial_log(summary);
    os64_printf("%s\n", summary + 9);

    return gFailed == 0 ? 0 : 1;
}
