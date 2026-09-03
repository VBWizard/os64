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

// The crime fixtures that PASS BY DYING: each commits a deliberate offense
// and the pass code is the badge the enforcement kills with. A crime that
// failed to kill would report the program's own exit code instead — which is
// exactly the failure worth catching, since a tripwire nobody tests is a
// tripwire nobody knows is disconnected.
static char *const argv_malloc_threads[]    = { "/tests/malloctest", "threads", "4", NULL };
static char *const argv_malloc_doublefree[] = { "/tests/malloctest", "doublefree", NULL };
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
    { "/tests/malloctest",      NULL, 0x0A110C00,  0,          "the heap: split, coalesce, recycle, give-back, /proc/<pid>/heap" },
    { "/tests/malloctest",      argv_malloc_threads,    0x0A110C10, 0, "four threads, one heap: the lock, and cross-thread frees" },
    { "/tests/malloctest",      argv_malloc_doublefree, 0xF12EEBAD, 0, "a double free kills the program (it must)" },
    { "/tests/malloctest",      argv_malloc_stomp,      0xCA9A12ED, 0, "a stomped canary kills the program (it must)" },
    { "/tests/nx_test",         argv_nx_stack, 139, 0,         "executing the stack kills the program (NX works)" },
    { "/tests/nx_test",         argv_nx_text,  139, 0,         "writing to .text kills the program (W^X works)" },
    { "/tests/fputest",         NULL, 0xF0DE0000,  0,          "x87/SSE data AND control state survive preemption, migration, a handler that wipes them, and a forged frame MXCSR" },
    // PASS BY DYING: a CPU exception from ring 3 ends the program with
    // 200 + vector (user_exception_kill), never the machine.
    // #XM is the one QEMU's TCG cannot raise (it records SSE exceptions in
    // MXCSR and never traps), so the fixture answers 3 there and that is a
    // SKIP, not a failure; on hardware and under KVM it dies with 219.
    { "/tests/fpfault",         argv_fpfault_xm, 219, 3,       "an unmasked SSE exception (#XM) kills the program, not the OS" },
    { "/tests/fpfault",         argv_fpfault_mf, 216, 0,       "an unmasked x87 exception (#MF) kills the program, not the OS" },
    { "/tests/fpfault",         argv_fpfault_de, 200, 0,       "an integer divide by zero (#DE) kills the program, not the OS" },
    { "/tests/fpfault",         argv_fpfault_ud, 206, 0,       "an AVX instruction with XSAVE off (#UD) kills the program, not the OS" },
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
    { "/tests/conftest",        NULL, 0x0C0F0000,  0x0C0F0001, "config library: get/get_bool/write/set, merge and atomic publish" },
    { "/tests/tartest",         NULL, 0x7A120000,  0,          "ustar create/list/extract, pipelines, atomic publication and path safety" },
    { "/tests/gziptest",        NULL, 0x621A0000,  0,          "standalone libgzip: streaming encode/decode and gzip/gunzip commands" },
    { "/tests/pngtest",         NULL, 0x90640000,  0,          "standalone libpng: filtered RGBA decode, alpha, CRC and pixel cap" },
    { "/tests/sigtest",         NULL, 0x05160000,  0,          "signal handlers: install, replace, restore, and the refusals" },
    { "/tests/winchtest",       NULL, 0x0A1D0000,  0,          "pty resize: the grid follows, the seats hear SIGWINCH, a blocked read and a blocked wait are interrupted" },
    { "/tests/df_test",         NULL, 0x0DF00000,  0,          "the direction flag does not cross a ring boundary (syscall, and into a handler)" },
    { "/tests/regleak_test",    NULL, 0x02E60000,  0,          "a syscall returns no kernel state in its scratch registers" },
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
