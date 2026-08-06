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
extern char kRootPartUUID[];
extern char kTZString[];
extern int kTSCCalibrationSeconds;
extern int kMaxActiveCores;
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
// SCHED=<mode>: scheduler mode selection. Absent means tickless — the default
// needs no flag, that's the point (2026-08-05 ruling; the misnamed BSPSCHED
// bool it replaces was removed the same day, no alias kept — all Limine
// entries migrated in the same commit). Recognized values are interpreted
// after the parse loop below; unknown values keep the tickless default and
// say so, because a typo silently landing you in the legacy mode is exactly
// the kind of quiet regression the default flip exists to prevent.
static char kSchedParam[16] = {0};

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
    {"LOGD", OPT_STRING, kLogdPath, 0, sizeof(kLogdPath)},
    {"SCHED", OPT_STRING, kSchedParam, 0, sizeof(kSchedParam)},
    {"NOTESTS", OPT_BOOL, &kRunTests, false, 0},
    {"NOUSB", OPT_BOOL, &kEnableUSB, false, 0},
    {"TESTPANIC", OPT_BOOL, &kTestPanic, true, 0},
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
}
