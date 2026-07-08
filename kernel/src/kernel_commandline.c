#include "kernel_commandline.h"
#include <stdbool.h>
#include "strings/strings.h"
#include <stdint.h>
#include "printd.h"
#include "kernel.h"

extern bool kOverrideFileLogging;
extern bool kEnableSMP;
extern bool kBspSchedulerMode;
extern bool kEnableKWorker;
extern bool kEnableGUI;
extern bool kRunTests;
extern char kRootPartUUID[];
extern int kMaxActiveCores;
bool kEnableAHCI = true, kEnableNVME = true;
// Off by default: the RAMDisk only activates when a boot entry passes BOTH
// the os64_disk.img module and the RAMDISK flag (see ramdisk.h).
bool kEnableRamdisk = false;

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
    {"BSPSCHED", OPT_BOOL, &kBspSchedulerMode, true, 0},
    {"NOTESTS", OPT_BOOL, &kRunTests, false, 0},
    {"KWORKER", OPT_BOOL, &kEnableKWorker, true, 0},
    {"GUI", OPT_BOOL, &kEnableGUI, true, 0},
    {"DEBUG_GUI", OPT_UINT128_OR, &kDebugLevel, DEBUG_GUI, 0},
    {"LOGFILE", OPT_BOOL, &kOverrideFileLogging, true, 0},
    {"ROOT", OPT_STRING, kRootPartUUID, 0, 64},
    // Older boot entries (VBox/Bosgame in limine.conf) still use the long
    // form; the strnstr-era parser accepted it, the table parser must too.
    {"ROOTPARTUUID", OPT_STRING, kRootPartUUID, 0, 64},
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
}
