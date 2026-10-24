// badptr — an address ring 3 cannot own is refused, never followed.
//
// THE RULE: the kernel copies a user buffer only after proving every byte of
// it lies below USER_CANONICAL_MAX (0x0000800000000000), the top of the
// canonical lower half, and is mapped there. An address at or above that
// line is either NON-CANONICAL — the CPU raises #GP on any access, and a #GP
// in ring 0 has no fixup, so it is a panic — or the kernel's own upper half.
//
// The bug this fixture was born for: the copy helpers bounded a user range by
// kHHDMOffset (0xffff800000000000) instead, so everything between the two
// passed. The page walk that comes next indexes with bits 39-47 and never
// looks at 48-63, so a non-canonical address walks SOME real table — the
// kernel's own at PML4[256..511] when bit 47 is set, which every task
// shares, or the task's own when it is clear — and answers "present", and
// the memcpy that followed took the machine down. One open() with a bad path
// was enough.
//
// Each case hands a syscall a pointer it cannot own and expects a refusal.
// A kernel that gets a COPY wrong does not fail this fixture: it panics, and
// the harness sees the boot die. A thread or program entry point that gets
// through is reported here as a failure, because under QEMU's TCG the bad RIP
// reaches ring 3 and only the program dies; hardware checks it in IRETQ,
// still in ring 0, and that #GP is the kernel's. /proc/self/mem walks
// without panicking, so a read past the ceiling there is reported as a
// failure too.
//
// What it does not reach: a pointer passed as a syscall argument is refused
// by the dispatcher's own bound before any copy helper sees it, so the
// helpers' bound is exercised only through EMBEDDED pointers (spawn's argv
// elements); and no case loads a program whose segment, rather than its
// entry, lies past the ceiling, so elf_map_segment's bound is untested here.
//
// `badptr <group>` runs one group, so each door can be shown on its own:
//   in      copy-in: open/stat/chdir paths, debug_log, a GUI title, a pipe
//           write, a net_dial struct
//   out     copy-out: read into a buffer, ticks, getcwd, stat's dirent
//   string  spawn's path and argv elements (the page-at-a-time measurer)
//   proc    /proc/self/mem: a seek past the ceiling, and a read at it
//   thread  a thread whose entry or exit stub is not a user address
//   entry   a program whose ELF entry point is not a user address: static,
//           dynamic, and position-independent (where only the entry plus the
//           load bias is out of range)
//   control (internal) what the unpatched copy of badptr that `entry` spawns
//           is asked to do: exit 0x600D
//
// Exit codes 0xBADA00NN ("BAD A(ddress)"): NN is the failing check, 0 = pass.

#include "os64/os64.h"
#include "os64/syscall.h"
#include "os64/syscall_numbers.h"
#include "os64/dirent.h"
#include "os64/net.h"
#include "os64/ticks.h"

#define BADGE         0xBADA0000u
#define CONTROL_CODE  0x600D

#define CEILING 0x0000800000000000ull   // USER_CANONICAL_MAX, the kernel's name for it

// Written once at startup so its page is PRESENT: the aliases below are only
// interesting if the walk they take lands on a real page.
static uint8_t gScratch[64];

typedef struct {
    uint64_t    addr;
    const char *what;
} bad_t;

#define BAD_COUNT 11
static bad_t gBad[BAD_COUNT];
static int   gBadCount;

static void add_bad(uint64_t addr, const char *what)
{
    gBad[gBadCount].addr = addr;
    gBad[gBadCount].what = what;
    gBadCount++;
}

static void build_bad_list(void)
{
    gScratch[0] = 1;
    uint64_t scratch = (uint64_t)(uintptr_t)gScratch;

    add_bad(CEILING,                 "the ceiling itself, the first non-canonical address");
    add_bad(0x0000ffff80000000ull,   "walks PML4[511]/PDPT[510], the kernel image's tables");
    add_bad(0x7fffffffffffffffull,   "the largest positive address");
    add_bad(0x8000000000000000ull,   "bit 63 alone");
    add_bad(scratch | 0x8000000000000000ull, "a present user page with bit 63 set (walks the task's own page)");
    add_bad(scratch | CEILING,       "a present user page with bit 47 set (walks a shared kernel-half table)");
    add_bad(CEILING - 8,             "8 bytes below the ceiling (a 16-byte range from here crosses it)");
    add_bad(0,                       "NULL");
    add_bad(0x10,                    "inside page 0");
    add_bad(0xffffffff80000000ull,   "the kernel image, canonical");
    add_bad(0xffff800000000000ull,   "the HHDM base, canonical");
}

static int gChecks;

// One refusal expected. A negative answer is a refusal whatever its code; the
// code is printed so a log shows WHICH refusal the kernel chose.
static void expect_refused(uint32_t step, const char *call, const bad_t *b, int64_t r)
{
    gChecks++;
    if (r < 0)
        return;
    os64_printf("badptr: %s(%p: %s) answered %ld - expected a refusal\n",
                call, (void *)(uintptr_t)b->addr, b->what, (long)r);
    os64_serial_log("badptr: FAIL - a bad pointer was accepted");
    os64_exit((int32_t)(BADGE | step));
}

// The serial beacon names the call BEFORE it is made: when a kernel without
// the bound dies, the last beacon on the wire says which door killed it.
// Only when a group is named: a full run inside testrun would otherwise put
// two hundred lines on the wire to say nothing went wrong.
static bool gVerbose;

static void beacon(const char *call, const bad_t *b)
{
    if (!gVerbose)
        return;
    char line[160];
    os64_snprintf(line, sizeof(line), "badptr: %s(0x%lx)", call, (unsigned long)b->addr);
    os64_serial_log(line);
}

static void group_in(void)
{
    int32_t p[2];
    if (os64_pipe(p) < 0)
        os64_exit((int32_t)(BADGE | 0x01));

    for (int i = 0; i < gBadCount; i++)
    {
        const bad_t *b = &gBad[i];
        const char *bp = (const char *)(uintptr_t)b->addr;

        beacon("open", b);
        expect_refused(0x10, "open", b, os64_open(bp, "r"));

        beacon("stat path", b);
        os64_dirent_t d;
        expect_refused(0x11, "stat", b, os64_stat(bp, &d));

        beacon("chdir", b);
        expect_refused(0x12, "chdir", b, os64_chdir(bp));

        beacon("debug_log", b);
        expect_refused(0x13, "debug_log", b,
                       (int64_t)os64_syscall2(SYSCALL_DEBUG_LOG, b->addr, 0));

        // The title is copied before the GUI is consulted, so this exercises
        // the copy on a text boot too: no desktop is needed to reach it.
        beacon("gui_window_create", b);
        expect_refused(0x14, "gui_window_create", b,
                       (int64_t)os64_syscall6(SYSCALL_GUI_WINDOW_CREATE, b->addr,
                                              0, 0, 64, 64, 0));

        // 16 bytes, so the range from CEILING - 8 crosses the ceiling.
        beacon("write", b);
        expect_refused(0x15, "write", b, os64_write(p[1], bp, 16));

        // The struct is copied before anything asks whether there is a
        // network, so a netless boot reaches the copy too.
        beacon("net_dial", b);
        expect_refused(0x16, "net_dial", b,
                       (int64_t)os64_syscall1(SYSCALL_NET_DIAL, b->addr));
    }

    os64_close(p[0]);
    os64_close(p[1]);
}

static void group_out(void)
{
    // Our own image: a file that certainly exists and has bytes to read.
    int64_t f = os64_open("/tests/badptr", "r");
    if (f < 0)
        os64_exit((int32_t)(BADGE | 0x02));

    for (int i = 0; i < gBadCount; i++)
    {
        const bad_t *b = &gBad[i];
        void *bp = (void *)(uintptr_t)b->addr;

        beacon("read", b);
        expect_refused(0x20, "read", b, os64_read((int32_t)f, bp, 16));

        beacon("ticks", b);
        expect_refused(0x21, "ticks", b, os64_ticks((os64_ticks_t *)bp));

        beacon("getcwd", b);
        expect_refused(0x22, "getcwd", b, os64_getcwd((char *)bp, 256));

        // A file that exists, so the copy-out is reached rather than a miss.
        beacon("stat dirent", b);
        expect_refused(0x23, "stat", b, os64_stat("/tests/badptr", (os64_dirent_t *)bp));
    }

    os64_close((int32_t)f);
}

// /proc/self/mem's offset is an address the READER chooses, and the kernel
// copies from it through the same page walk: the stream ends at the ceiling.
// A seek past it is refused, and a read at it returns nothing.
static void group_proc(void)
{
    int64_t f = os64_open("/proc/self/mem", "r");
    if (f < 0)
        os64_exit((int32_t)(BADGE | 0x03));

    static const bad_t past[] = {
        { CEILING + 0x100000ull,  "1 MiB past the ceiling" },
        { 0x0000ffff80000000ull,  "non-canonical, bit 47 set" },
    };
    for (unsigned i = 0; i < sizeof(past) / sizeof(past[0]); i++)
    {
        beacon("proc mem seek", &past[i]);
        expect_refused(0x30, "seek /proc/self/mem", &past[i],
                       os64_seek((int32_t)f, (int64_t)past[i].addr, OS64_SEEK_SET));
    }

    // AT the ceiling the seek is legal (the end of the stream) and the read
    // finds nothing there.
    gChecks++;
    uint8_t buf[16];
    if (os64_seek((int32_t)f, (int64_t)CEILING, OS64_SEEK_SET) < 0 ||
        os64_read((int32_t)f, buf, sizeof(buf)) > 0)
    {
        os64_printf("badptr: /proc/self/mem gave bytes at the ceiling\n");
        os64_serial_log("badptr: FAIL - /proc/self/mem read past the task's address space");
        os64_exit((int32_t)(BADGE | 0x31));
    }

    os64_close((int32_t)f);
}

static void group_string(void)
{
    for (int i = 0; i < gBadCount; i++)
    {
        const bad_t *b = &gBad[i];

        beacon("spawn path", b);
        char *const solo[] = { "x", NULL };
        expect_refused(0x30, "spawn", b,
                       os64_spawn((const char *)(uintptr_t)b->addr, solo));

        // NULL is argv's TERMINATOR, not a bad pointer: an argv of {NULL} and
        // an argv pointer of NULL are both "no arguments", and would spawn.
        if (b->addr == 0)
            continue;

        beacon("spawn argv[1]", b);
        char *const argv[] = { "/tests/badptr", (char *)(uintptr_t)b->addr, NULL };
        expect_refused(0x31, "spawn argv[1]", b, os64_spawn("/tests/badptr", argv));

        beacon("spawn argv", b);
        expect_refused(0x32, "spawn argv", b,
                       os64_spawn("/tests/badptr", (char *const *)(uintptr_t)b->addr));
    }
}

extern void os64_thread_trampoline(void);

static int64_t never_runs(void *arg)
{
    (void)arg;
    return 0;
}

// Only the addresses at or above the ceiling: one below it is a legal user
// address the kernel rightly accepts, and a thread started there would fault
// in ring 3 and take this program with it.
static void group_thread(void)
{
    // An ENTRY the CPU cannot return to: on hardware IRETQ checks the new
    // RIP before it leaves ring 0, so a non-canonical one is a #GP in the
    // kernel. (Under TCG the thread dies in ring 3 instead, taking this
    // program with it — a failure either way.)
    for (int i = 0; i < gBadCount; i++)
    {
        const bad_t *b = &gBad[i];
        if (b->addr < CEILING)
            continue;
        beacon("thread entry", b);
        expect_refused(0x40, "thread entry", b,
                       (int64_t)os64_syscall3(SYSCALL_THREAD, b->addr, 0,
                                              (uint64_t)(uintptr_t)&os64_thread_trampoline));
    }

    // An EXIT STUB the thread would `ret` into. That one would only kill this
    // program (the fault is at the ret, in ring 3), but it is the same bound,
    // and it runs last because a kernel without it ends the fixture here.
    for (int i = 0; i < gBadCount; i++)
    {
        const bad_t *b = &gBad[i];
        if (b->addr < CEILING)
            continue;
        beacon("thread exit_stub", b);
        expect_refused(0x41, "thread exit_stub", b,
                       (int64_t)os64_syscall3(SYSCALL_THREAD,
                                              (uint64_t)(uintptr_t)&never_runs, 0, b->addr));
    }
}

// ── entry: a program whose e_entry is not a user address ────────────────────

static uint8_t gImage[1u << 20];

static int64_t slurp(const char *path)
{
    int64_t f = os64_open(path, "r");
    if (f < 0)
        return -1;
    int64_t total = 0;
    for (;;)
    {
        if (total == (int64_t)sizeof(gImage))
        {
            os64_close((int32_t)f);
            return -1;   // bigger than the buffer: say so rather than truncate
        }
        int64_t n = os64_read((int32_t)f, gImage + total, sizeof(gImage) - (size_t)total);
        if (n < 0)
        {
            os64_close((int32_t)f);
            return -1;
        }
        if (n == 0)
            break;
        total += n;
    }
    os64_close((int32_t)f);
    return total;
}

static bool spill(const char *path, int64_t len)
{
    int64_t f = os64_open(path, "w");
    if (f < 0)
        return false;
    bool ok = os64_write((int32_t)f, gImage, (size_t)len) == len;
    return os64_close((int32_t)f) == 0 && ok;
}

// e_entry is the eight bytes at offset 24 of an ELF64 header.
static void set_entry(uint64_t entry)
{
    for (int i = 0; i < 8; i++)
        gImage[24 + i] = (uint8_t)(entry >> (8 * i));
}

static int32_t run_and_wait(const char *path, char *const argv[], int64_t *pid_out)
{
    int64_t pid = os64_spawn(path, argv);
    *pid_out = pid;
    if (pid < 0)
        return 0;
    int32_t code = 0;
    if (os64_wait(pid, &code) < 0)
        os64_exit((int32_t)(BADGE | 0x03));
    return code;
}

// One patched copy of the image in gImage, at its OWN path: the kernel keeps
// a dynamically linked image resident by path and file identity, and a copy
// rewritten in place could be answered from the previous one's cache.
static void patched_run(uint32_t step, const char *src, const char *stem, const char *arg,
                        int64_t len, int index, const bad_t *b)
{
    char path[64];
    int64_t pid;
    os64_snprintf(path, sizeof(path), "%s.%d", stem, index);
    char *const argv[] = { path, (char *)arg, NULL };
    set_entry(b->addr);
    if (!spill(path, len))
        os64_exit((int32_t)(BADGE | (step + 1)));
    beacon(src, b);
    (void)run_and_wait(path, argv, &pid);
    os64_unlink(path);
    expect_refused(step + 3, "spawn (entry)", b, pid);
}

// Copy `src` under /tmp: unpatched first, which must RUN and exit with `want`
// (so the refusals after it are about the entry point and nothing else), then
// once per address no CPU can enter in ring 3, with only e_entry changed,
// which must not spawn at all. `biased`, when not NULL, is one more entry: a
// value below the ceiling that a position-independent image's load bias
// carries over it.
static void entry_case(uint32_t step, const char *src, const char *stem,
                       const char *arg, int32_t want, const bad_t *biased)
{
    int64_t len = slurp(src);
    if (len < 64)
        os64_exit((int32_t)(BADGE | step));

    char path[64];
    int64_t pid;
    os64_snprintf(path, sizeof(path), "%s.control", stem);
    char *const control_argv[] = { path, (char *)arg, NULL };
    if (!spill(path, len))
        os64_exit((int32_t)(BADGE | (step + 1)));
    int32_t got = run_and_wait(path, control_argv, &pid);
    os64_unlink(path);
    if (pid < 0 || got != want)
    {
        os64_printf("badptr: the unpatched copy of %s answered pid %ld exit 0x%x (want 0x%x)\n",
                    src, (long)pid, (unsigned)got, (unsigned)want);
        os64_exit((int32_t)(BADGE | (step + 2)));
    }

    for (int i = 0; i < gBadCount; i++)
        if (gBad[i].addr >= CEILING)
            patched_run(step, src, stem, arg, len, i, &gBad[i]);
    if (biased != NULL)
        patched_run(step, src, stem, arg, len, gBadCount, biased);
}

static void group_entry(void)
{
    // A static, demand-paged image: elf_load_from_file's door.
    entry_case(0x50, "/tests/test_elf", "/tmp/badptr.static", NULL, (int32_t)0xE1F0CA11, NULL);

    // A dynamically linked one, the shape every real program has:
    // elf_resolve_dynamic_dependencies' door. Ourselves, asked for `control`.
    entry_case(0x58, "/tests/badptr", "/tmp/badptr.dynamic", "control", CONTROL_CODE, NULL);

    // A position-independent one, whose entry is load_bias + e_entry. The
    // bias is a slot in the shared-library window, far up the lower half, so
    // an e_entry one page under the ceiling is a legal-looking header field
    // whose SUM is not a user address: only the check on the sum can see it.
    static const bad_t biased = { CEILING - 0x1000, "one page under the ceiling, before the load bias" };
    entry_case(0x60, "/tests/dyn_consumer", "/tmp/badptr.pie", NULL, 0x00300031, &biased);
}

// In a full run, one line per group reaches the wire, so a kernel that dies
// mid-run is still named to the group.
static bool start(const char *only, const char *group)
{
    if (only != NULL && !os64_streq(only, group))
        return false;
    char line[64];
    os64_snprintf(line, sizeof(line), "badptr: group %s", group);
    os64_serial_log(line);
    return true;
}

int main(int argc, char **argv)
{
    const char *only = argc > 1 ? argv[1] : NULL;
    if (only != NULL && os64_streq(only, "control"))
        return CONTROL_CODE;

    build_bad_list();
    gVerbose = (only != NULL);

    bool ran = false;
    if (start(only, "in"))     { group_in();     ran = true; }
    if (start(only, "out"))    { group_out();    ran = true; }
    if (start(only, "string")) { group_string(); ran = true; }
    if (start(only, "proc"))   { group_proc();   ran = true; }
    if (start(only, "entry"))  { group_entry();  ran = true; }
    // Last: on a kernel without the bound, the exit-stub half ends this
    // program rather than the machine.
    if (start(only, "thread")) { group_thread(); ran = true; }

    if (!ran)
    {
        os64_printf("usage: badptr [in|out|string|proc|thread|entry]\n");
        return 2;
    }

    char line[96];
    os64_snprintf(line, sizeof(line), "badptr: %d bad pointers refused, none followed", gChecks);
    os64_serial_log(line);
    os64_printf("%s\n", line);
    return (int)BADGE;
}
