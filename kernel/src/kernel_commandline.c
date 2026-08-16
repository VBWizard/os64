#include "kernel_commandline.h"
#include <stdbool.h>
#include "strings/strings.h"
#include <stdint.h>
#include "printd.h"
#include "kernel.h"

extern bool kOverrideFileLogging;
extern bool kEnableSMP;
extern bool kTicklessScheduler;
extern bool kEnableKWorker;
extern bool kEnableGUI;
extern bool kRunTests;
extern bool kEnableUSB;
extern bool kEnableNet;
extern bool kEnableR8125;
// Static IPv4 configuration strings (ipv4.c owns them; empty = the 10.0.2.x
// NAT-convention defaults shared by QEMU slirp and VirtualBox NAT). DHCP
// supersedes all three in NETWORK.md Phase 3.
extern char kNetIPString[];
extern char kNetGWString[];
extern char kNetMaskString[];
extern char kRootPartUUID[];
extern char kTZString[];
extern int kTSCCalibrationSeconds;
extern int kMaxActiveCores;
extern int kBlockCacheCapMB;      // block_cache.c — CACHE=<MB>
extern bool kBlockCacheDisabled;  // block_cache.c — NOCACHE
bool kEnableAHCI = true, kEnableNVME = true;
// Off by default: the RAMDisk only activates when a boot entry passes BOTH
// the os64_disk.img module and the RAMDISK flag (see ramdisk.h).
bool kEnableRamdisk = false;
// BOOTMARK boot-phase mile-markers (kernel.h) — off by default so the boot
// screen stays clean; flip on per-entry when timing a boot.
bool kEnableBootmarks = false;
// TEMP (userland bring-up): launch /bin/hello as a normal scheduled app from
// the boot flow (not the test harness). Remove once the shell exists — it's
// the stand-in for "launch init/shell". Off by default.
bool kRunHello = false;
// TEMP (read-syscall bring-up): launch /bin/keytest and keep the system up so
// it can block on read(0) and echo injected keys. Remove with keytest.
bool kRunKeytest = false;
// Launch /bin/husk (the shell) from the boot flow and keep the system up. This
// is the real "launch the shell" path; the HELLO/KEYTEST temps fold into it.
bool kRunHusk = false;
// TESTRUN: launch /bin/testrun (the ring-3 half of the suite) once the boot
// flow is done. The in-kernel suite can only test what the kernel can see;
// the fixtures that answer "does a program actually run, exit, and hand back
// the right code" had to become a PROGRAM, so they moved out here. Off by
// default and flag-gated like TESTPANIC/SHUTDOWNTEST — it spawns and waits on
// a dozen fixtures, which is not something an interactive boot should pay for.
// Its verdict line ("TESTRUN: N passed, M failed, K skipped") goes to the
// serial wire via klog, so an unattended A/B run can grep it — which is the
// entire reason the A/B harness limine entry exists.
bool kRunTestrun = false;
// DIRECTLOG: printd writes STRAIGHT to COM1 with the polled writer, bypassing
// the per-core queues entirely.
//
// The queues are the right design and stay the default — but they have one
// blind spot, and it is precisely the spot you need a log most. The backlog is
// drained by logd, and logd is a TASK: it cannot run until the scheduler does.
// So a boot that dies at or before scheduler start emits only the handful of
// lines written before kLoggingInitialized flipped, and then goes silent
// forever. That is not a hypothetical — it is exactly what a VBox boot freezing
// after the pre-boot tests looks like (2026-08-10): five lines at timestamp 0,
// ending mid-init, twice, with the real story never reaching the wire.
//
// This flag makes the pre-init bootstrap path — which already existed, as the
// `else` branch below kLoggingInitialized — available for the whole boot. It is
// SLOW on purpose: polled 16550 writes at 115200 baud, no batching, the calling
// core spinning on THRE for every byte. That is a terrible way to run an OS and
// a superb way to debug one, because a line that reached the wire cannot be lost
// by whatever dies next. Never a default; reach for it when the machine stops
// talking.
bool kDirectLog = false;
// NOTRACE: the kill switch on symbolized fault reports (stack_trace.c).
//
// ON by default, because a crash that names its own call chain is worth far
// more than the few KB per program it costs. Off-switchable from day one for
// one reason, and it is a good one (Chris's requirement, and I would have asked
// for it): a crash reporter that can itself fault is the WORST possible bug —
// it converts a diagnosable segfault into a triple fault with no output at all,
// leaving you strictly worse off than printing nothing. Until the walker has
// earned trust on real hardware, `NOTRACE` gets the machine back to the old
// behavior without a rebuild.
//
// It gates the COST as well as the risk: with NOTRACE set, elf_load never reads
// .symtab/.strtab at all, so a program pays nothing — no parse, no memory, no
// walker. Same doctrine as SCHED=periodic and NOCACHE: every new mechanism
// keeps its flashlight and its off switch.
bool kEnableStackTrace = true;
// LOGD=<path>: the file a ring-3 log daemon should append the kernel log to.
// Non-empty means TWO things, and both matter from the very first log line:
// the kernel launches /bin/logd with this path as soon as a filesystem exists
// (before the tests, which is the loud part), and until that daemon attaches
// the kernel drainer stays OFF the serial port and simply lets the rings fill
// (see LOG_SINK_AWAIT_* in log.h).
//
// It lives on the commandline rather than in a config file because os64 has
// no configuration yet — and the kernel already launches /bin/husk and
// kworker this way, so logd is not a new kind of citizen. When real config
// arrives this becomes one line in a file and the flag can retire.
char kLogdPath[128] = {0};
// Panic ON PURPOSE right after the post-boot tests: the standing diagnostic
// for the panic pipeline itself. A panic's dying-breath serial path (direct
// write + logd emergency flush, see panic.c) is exactly the kind of code that
// silently regresses — it only runs when everything is already going wrong —
// so this flag makes it a one-boot test: boot with TESTPANIC, then check the
// log ends with the banner, the flushed backlog, and the repeated message.
bool kTestPanic = false;
// SHUTDOWNTEST's flag — same one-boot-diagnostic idea for the descent.
bool kTestShutdown = false;
// NMIPROBE: sweep every other core with a diagnostic NMI right after the
// post-boot tests. Same one-boot-diagnostic pattern as TESTPANIC, and for the
// same reason — the NMI probe (nmi_probe.c) only ever runs when something has
// already gone wrong, which is exactly the kind of code that rots unnoticed.
// On a HEALTHY machine every core answers in microseconds and goes straight
// back to what it was doing, so a clean sweep here proves the whole path:
// delivery, the IST2 stack switch, capture, and above all the resume.
bool kTestNmiProbe = false;
// TESTPF: dereference a deliberately unmapped KERNEL address right after the
// post-boot tests, to exercise the fatal page-fault report itself.
//
// Third member of the TESTPANIC family, and it exists for the same reason as
// the other two: this code path only ever runs when something has already gone
// badly wrong, which is precisely the kind of code that regresses in silence.
// It earned its place the day it was written — the fatal #PF report had been
// quietly thinner than every other exception's for as long as it had existed
// (no registers, no AP/thread, no CR3), and nobody noticed until a real fault
// during a VT soak put the two side by side.
//
// Kernel-mode and no VMA on purpose: that is the exact path a wild kernel
// pointer takes, and the one Chris's soak fault took.
bool kTestPageFault = false;
// TESTWATCH: arm a hardware watchpoint on a bait variable and store to it,
// twice — once in TRACE mode (report and keep running) and once in HALT mode.
// Fifth member of the TESTPANIC family; proves DR programming, the #DB path,
// slot attribution, the named report, and above all that a traced hit RESUMES.
bool kTestWatchpoint = false;
// WATCHDMA: arm a hardware watchpoint on each NVMe controller's write DMA
// bounce buffer PAGE TABLE ENTRY (nvme.c). Born 2026-08-14 for the P5
// corruption, and general on purpose: any mapping that must never change can
// be watched the same way with WATCH=<addr> instead.
bool kWatchDMA = false;
// TESTGP: raise a deliberate #GP (a non-canonical dereference) right after the
// post-boot tests — fourth member of the TESTPANIC family.
//
// It exists to prove the one specific thing the unified exception path was
// built for: that a #GP reports ITS OWN registers. Under the old stubs, only
// #PF captured registers, and every other vector printed whatever the last
// page fault left behind — a bug that survived precisely because nobody ever
// raised a #GP on purpose and checked. The test plants signature values in
// RAX and R15 before faulting; if the report shows them, the capture is real.
bool kTestGPFault = false;
// EXCOLD: wire the pre-2026-08-11 per-vector exception stubs and reporters
// instead of the unified path (exception_entry.S + exception_report.c).
//
// The fallback is a RUNTIME switch rather than a git revert for one reason: if
// the new path is broken, the plausible failure mode is a fault inside the
// fault reporter — no output at all — which is exactly when you cannot afford
// a rebuild cycle to get a machine that talks. Boot EXCOLD, compare, diagnose.
// Same doctrine as SCHED=periodic, NOCACHE, NOTRACE: every new mechanism keeps
// its off switch until it has earned trust on real hardware. The old stubs and
// this flag retire together when that day comes.
bool kUseOldExceptions = false;
extern char kTestsPolicyOverride[];
// SCHED=<mode>: scheduler mode selection. Absent means tickless — the default
// needs no flag, that's the point (2026-08-05 ruling; the misnamed BSPSCHED
// bool it replaces was removed the same day, no alias kept — all Limine
// entries migrated in the same commit). Recognized values are interpreted
// after the parse loop below; unknown values keep the tickless default and
// say so, because a typo silently landing you in the legacy mode is exactly
// the kind of quiet regression the default flip exists to prevent.
static char kSchedParam[16] = {0};
extern char kWatchSpec[128];   // watchpoint.c owns it; the table below fills it

// -----------------------------------------------------------------------
// Kernel command-line parser definitions
// -----------------------------------------------------------------------

typedef enum
{
    OPT_BOOL,
    OPT_UINT128_CLEAR,
    OPT_UINT128_OR,
    OPT_STRING,
    OPT_INT      // NAME=<decimal>, stored to an int
} opt_type_t;

typedef struct
{
    const char *name;
    opt_type_t type;
    void *dest;
    __uint128_t flagmask; // for UINT128 or bool
    size_t maxlen;        // for string buffers
} cmdopt_t;

#define MAX_CMDLINE_TOKENS 64

// Minimal decimal parser for OPT_INT values (no atoi in the kernel string
// lib). Returns -1 on empty/non-numeric input so callers can reject it.
static int parse_decimal(const char *s)
{
    int value = 0;
    if (!*s)
        return -1;
    while (*s)
    {
        if (*s < '0' || *s > '9')
            return -1;
        value = value * 10 + (*s - '0');
        s++;
    }
    return value;
}

// Simple in-place whitespace tokenizer
static int tokenize(char *cmdline, const char **argv, int max)
{
    int argc = 0;
    char *p = cmdline;
    while (*p && argc < max)
    {
        while ((*p == ' ' || *p == '\t') && *p)
            p++;
        if (!*p)
            break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t')
            p++;
        if (*p)
        {
            *p = '\0';
            p++;
        }
    }
    return argc;
}

// Table of all recognized switches / key=value pairs
static cmdopt_t cmdopts[] = {
    // Name/Type/Dest/FlagMask/MaxLen
    {"nolog", OPT_UINT128_CLEAR, &kDebugLevel, 0, 0},
    {"alllog", OPT_UINT128_OR, &kDebugLevel, DEBUG_EVERYTHING, 0},
    {"nosmp", OPT_BOOL, &kEnableSMP, false, 0},
    // NOBACKSTOP: disarm the tickless AP preemption lease (kernel.c doctrine
    // block) — restores the pre-2026-08-13 park-and-nudge tickless exactly.
    {"NOBACKSTOP", OPT_BOOL, &kSchedBackstopEnabled, false, 0},
    // BACKSTOP=<ms>: the lease length for this boot (default SCHED_BACKSTOP_MS
    // = 50; GUI entries pass 10 for smooth animation). Validated after the
    // parse loop — out-of-range snaps back to the default, loudly.
    {"BACKSTOP", OPT_INT, &kSchedBackstopMS, 0, 0},
    // Cap the number of cores init_SMP brings up (0 = use them all). The
    // uncapped cores are never woken — they stay parked in Limine's AP loop.
    {"MAXCORES", OPT_INT, &kMaxActiveCores, 0, 0},
    {"DEBUG_DETAILED", OPT_UINT128_OR, &kDebugLevel, DEBUG_DETAILED, 0},
    {"DEBUG_EXTRA_DETAILED", OPT_UINT128_OR, &kDebugLevel, DEBUG_DETAILED | DEBUG_EXTRA_DETAILED, 0},
    {"AHCI", OPT_BOOL, &kEnableAHCI, true, 0},
    {"NOAHCI", OPT_BOOL, &kEnableAHCI, false, 0},
    {"NVME", OPT_BOOL, &kEnableNVME, true, 0},
    {"NONVME", OPT_BOOL, &kEnableNVME, false, 0},
    {"RAMDISK", OPT_BOOL, &kEnableRamdisk, true, 0},
    {"BOOTMARKS", OPT_BOOL, &kEnableBootmarks, true, 0},
    {"HELLO", OPT_BOOL, &kRunHello, true, 0},
    {"KEYTEST", OPT_BOOL, &kRunKeytest, true, 0},
    {"HUSK", OPT_BOOL, &kRunHusk, true, 0},
    {"TESTRUN", OPT_BOOL, &kRunTestrun, true, 0},
    {"DIRECTLOG", OPT_BOOL, &kDirectLog, true, 0},
    {"NOTRACE", OPT_BOOL, &kEnableStackTrace, false, 0},
    {"LOGD", OPT_STRING, kLogdPath, 0, sizeof(kLogdPath)},
    {"SCHED", OPT_STRING, kSchedParam, 0, sizeof(kSchedParam)},
    // WATCH=<hexaddr>[:len[:kind[:action]]] — arm a hardware watchpoint at
    // boot. One string rather than four flags because a watchpoint is one
    // idea; the grammar (and its defaults: 8 bytes, on write, halt on hit)
    // lives with the parser in watchpoint.c.
    {"WATCH", OPT_STRING, kWatchSpec, 0, sizeof(kWatchSpec)},
    {"NOTESTS", OPT_BOOL, &kRunTests, false, 0},
    // The buffer cache's two knobs (block_cache.h): CACHE=<MB> sizes the
    // read-cache budget (default 64; 0 = off), NOCACHE is the diagnostic
    // flashlight — same doctrine as SCHED=periodic, every new default keeps
    // an honest off-switch.
    {"CACHE", OPT_INT, &kBlockCacheCapMB, 0, 0},
    {"NOCACHE", OPT_BOOL, &kBlockCacheDisabled, true, 0},
    {"NOUSB", OPT_BOOL, &kEnableUSB, false, 0},
    {"NONET", OPT_BOOL, &kEnableNet, false, 0},
    // The RTL8125 flashlight, sibling of NOAHCI/NONVME. UPPERCASE because
    // this table is matched with strcmp and folds no case — the lesson
    // three dead boot entries taught on 2026-08-16 (commit 642eb9f).
    {"NOR8125", OPT_BOOL, &kEnableR8125, false, 0},
    {"DEBUG_NET", OPT_UINT128_OR, &kDebugLevel, DEBUG_NET, 0},
    // The NVMe command-stream histogram (nvme.c iostat) rides this level —
    // boot with DEBUG_NVME, run the workload, read the log.
    {"DEBUG_NVME", OPT_UINT128_OR, &kDebugLevel, DEBUG_NVME, 0},
    // The allocator health line (kworker cadence, ~10s: table entry counts +
    // exactfit/split/merge/compaction counters + top-4 free-hole sizes). The
    // walk itself is gated on this bit, so the flag truly costs nothing when
    // off — Chris's requirement the day the status table hit 12k entries.
    {"DEBUG_ALLOCATOR", OPT_UINT128_OR, &kDebugLevel, DEBUG_ALLOCATOR, 0},
    // The SATA pair, born on the P5's first disk-root attempt (2026-08-08):
    // every AHCI line was printd(DEBUG_AHCI) and the bit was in no default
    // mask, so "did the driver see my disk" and "the driver never ran" both
    // read as an empty grep. Bare metal diagnoses through these.
    {"DEBUG_AHCI", OPT_UINT128_OR, &kDebugLevel, DEBUG_AHCI, 0},
    {"DEBUG_HARDDRIVE", OPT_UINT128_OR, &kDebugLevel, DEBUG_HARDDRIVE, 0},
    // Static IPv4 config, dotted-quad (e.g. IP=192.168.1.50). Parsed by
    // ipv4_config_init; a malformed value falls back to the default.
    {"IP", OPT_STRING, kNetIPString, 0, 20},
    {"GW", OPT_STRING, kNetGWString, 0, 20},
    {"MASK", OPT_STRING, kNetMaskString, 0, 20},
    {"TESTPANIC", OPT_BOOL, &kTestPanic, true, 0},
    {"NMIPROBE", OPT_BOOL, &kTestNmiProbe, true, 0},
    {"TESTPF", OPT_BOOL, &kTestPageFault, true, 0},
    {"TESTGP", OPT_BOOL, &kTestGPFault, true, 0},
    {"TESTWATCH", OPT_BOOL, &kTestWatchpoint, true, 0},
    {"WATCHDMA", OPT_BOOL, &kWatchDMA, true, 0},
    {"EXCOLD", OPT_BOOL, &kUseOldExceptions, true, 0},
    // TESTS=panic|warn — one-boot override of every test's failure policy
    // (test_framework.h owns the taxonomy: warn / remount-ro / panic, the
    // ext2 s_errors trio reborn). Unset honors each test's registration.
    {"TESTS", OPT_STRING, kTestsPolicyOverride, 0, 8},
    // SHUTDOWNTEST — run the full shutdown descent after the post-boot
    // tests (same diagnostic pattern as TESTPANIC): logd retire, sync_all,
    // NVMe FLUSH, poweroff. Under QEMU the process exiting IS the pass.
    {"SHUTDOWNTEST", OPT_BOOL, &kTestShutdown, true, 0},
    {"KWORKER", OPT_BOOL, &kEnableKWorker, true, 0},
    {"GUI", OPT_BOOL, &kEnableGUI, true, 0},
    {"DEBUG_GUI", OPT_UINT128_OR, &kDebugLevel, DEBUG_GUI, 0},
    {"DEBUG_USB", OPT_UINT128_OR, &kDebugLevel, DEBUG_USB, 0},
    // Pipes: lifecycle + every refcount change + every park/wake + EOF/EPIPE.
    // Add DEBUG_DETAILED alongside it for the byte-by-byte data flow.
    {"DEBUG_PIPE", OPT_UINT128_OR, &kDebugLevel, DEBUG_PIPE, 0},
    // Demand pager: fault announce + resolution (mapped/CoW), two lines per
    // fault. Add DEBUG_DETAILED for the per-fault VMA lookup detail.
    // Resolved faults are NOT on DEBUG_EXCEPTIONS anymore — opt in here.
    {"DEBUG_DEMAND_PAGING", OPT_UINT128_OR, &kDebugLevel, DEBUG_DEMAND_PAGING, 0},
    // Task lifecycle: creation, the undertaker's burials, and the per-burial
    // VMA-backing announcement — born 2026-08-13 as the DEFERRED RECLAIM line
    // (pages counted, not freed), reworded 2026-08-15 when the deferral was
    // paid (pages freed, reclaim announced). That announcement is why this
    // switch exists at all: the channel had no way to be turned on for a
    // single boot, so the loudest thing on it could not be heard without
    // recompiling. Add DEBUG_DETAILED for per-release refcount lines.
    {"DEBUG_TASK", OPT_UINT128_OR, &kDebugLevel, DEBUG_TASK, 0},
    {"LOGFILE", OPT_BOOL, &kOverrideFileLogging, true, 0},
    {"ROOT", OPT_STRING, kRootPartUUID, 0, 64},
    // Older boot entries (VBox/Bosgame in limine.conf) still use the long
    // form; the strnstr-era parser accepted it, the table parser must too.
    {"ROOTPARTUUID", OPT_STRING, kRootPartUUID, 0, 64},
    // Timezone, classic TZ format (TZ=EST5EDT). Stored verbatim; see the
    // declaration in kernel.c for who consumes which half of it.
    {"TZ", OPT_STRING, kTZString, 0, 64},
    // Boot TSC calibration window in seconds (see kernel.c for the default
    // and the precision arithmetic). TSCCAL=5 for the impatient.
    {"TSCCAL", OPT_INT, &kTSCCalibrationSeconds, 0, 0},
};

void process_kernel_commandline(char *cmdline)
{
    const char *argv[MAX_CMDLINE_TOKENS];
    int argc = tokenize(cmdline, argv, MAX_CMDLINE_TOKENS);

    for (unsigned i = 0; i < sizeof(cmdopts) / sizeof(cmdopt_t); i++)
    {
        cmdopt_t *opt = &cmdopts[i];

        for (int argnum = 0; argnum < argc; argnum++)
        {
            const char *arg = argv[argnum];
            if (opt->type == OPT_STRING)
            {
                size_t name_len = strlen(opt->name);
                if (strncmp(arg, opt->name, name_len) == 0 && arg[name_len] == '=')
                {
                    strncpy(opt->dest, arg + name_len + 1, opt->maxlen);
                    ((char *)opt->dest)[opt->maxlen - 1] = '\0';
                    break;
                }
            }
            else if (opt->type == OPT_INT)
            {
                size_t name_len = strlen(opt->name);
                if (strncmp(arg, opt->name, name_len) == 0 && arg[name_len] == '=')
                {
                    int value = parse_decimal(arg + name_len + 1);
                    // Reject garbage (parse_decimal returns -1) but keep the
                    // option's compiled-in default rather than guessing.
                    if (value >= 0)
                        *(int *)opt->dest = value;
                    break;
                }
            }
            else if (strcmp(arg, opt->name) == 0)
            {
                switch (opt->type)
                {
                case OPT_BOOL:
                    *(bool *)opt->dest = (bool)opt->flagmask;
                    break;
                case OPT_UINT128_CLEAR:
                    *(__uint128_t *)opt->dest = 0;
                    break;
                case OPT_UINT128_OR:
                    *(__uint128_t *)opt->dest |= opt->flagmask;
                    break;
                default:
                    break;
                }
                break;
            }
        }
    }

    // Interpret SCHED= now that the table pass has filled kSchedParam.
    // "periodic" is the whole menu today; new policies slot in here as the
    // tickless arc adds them (one-shot quanta, etc.). Deliberately NOT a
    // table entry per value: the values share one destination variable and
    // the unknown-value warning needs a home.
    if (kSchedParam[0])
    {
        if (strcmp(kSchedParam, "periodic") == 0)
            kTicklessScheduler = false;
        else
            printd(DEBUG_BOOT, "cmdline: unknown SCHED=%s ignored, staying tickless\n", kSchedParam);
    }

    // Validate BACKSTOP= the same way: a lease outside 1..1000ms is a typo,
    // not a policy. 0 (or parse_decimal's -1 for garbage) would arm a
    // zero-count timer — which SDM-stops it, silently recreating the
    // starvation bug behind a flag that LOOKS like it enabled something.
    // Snap back to the default and say so; NOBACKSTOP is the honest way to
    // turn the lease off.
    if (kSchedBackstopMS < 1 || kSchedBackstopMS > 1000)
    {
        printd(DEBUG_BOOT, "cmdline: BACKSTOP=%d out of range (1..1000ms), using default %dms\n",
               kSchedBackstopMS, SCHED_BACKSTOP_MS);
        kSchedBackstopMS = SCHED_BACKSTOP_MS;
    }
}
