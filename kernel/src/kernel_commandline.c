#include "kernel_commandline.h"
#include <stdbool.h>
#include "strings/strings.h"
#include <stdint.h>
#include "printd.h"
#include "kernel.h"

extern bool kOverrideFileLogging;
extern char kRootPartUUID[];
bool kEnableAHCI = true, kEnableNVME = true;

// -----------------------------------------------------------------------
// Kernel command-line parser definitions
// -----------------------------------------------------------------------

typedef enum
{
    OPT_BOOL,
    OPT_UINT128_CLEAR,
    OPT_UINT128_OR,
    OPT_STRING
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

// Simple in-place whitespace tokenizer
static int tokenize(char *cmdline, char **argv, int max)
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
   //Name/Type/Dest/FlagMask/MaxLen
    {"nolog", OPT_UINT128_CLEAR, &kDebugLevel, 0, 0},
    {"alllog", OPT_UINT128_OR, &kDebugLevel, DEBUG_EVERYTHING, 0},
    {"DEBUG_DETAILED", OPT_UINT128_OR, &kDebugLevel, DEBUG_DETAILED, 0},
    {"DEBUG_EXTRA_DETAILED", OPT_UINT128_OR, &kDebugLevel, DEBUG_DETAILED | DEBUG_EXTRA_DETAILED, 0},
    {"AHCI", OPT_BOOL, &kEnableAHCI, true, 0},
    {"NOAHCI", OPT_BOOL, &kEnableAHCI, false, 0},
    {"NVME", OPT_BOOL, &kEnableNVME, true, 0},
    {"NONVME", OPT_BOOL, &kEnableNVME, false, 0},
    {"LOGFILE", OPT_BOOL, &kOverrideFileLogging, true, 0},
    {"ROOT", OPT_STRING, kRootPartUUID, 0, 64},
};

void process_kernel_commandline(char *cmdline)
{
    char *argv[MAX_CMDLINE_TOKENS];
    int argc = tokenize(cmdline, argv, MAX_CMDLINE_TOKENS);

    for (unsigned i = 0; i < sizeof(cmdopts) / sizeof(cmdopt_t); i++)
    {
        cmdopt_t *opt = &cmdopts[i];

        for (int j = 0; j < argc; j++)
        {
            char *arg = argv[j];
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
