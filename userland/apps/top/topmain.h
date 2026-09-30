#ifndef TOPMAIN_H
#define TOPMAIN_H
#include "os64/os64.h"

// Summary percentages describe the whole machine; process rows describe
// one CPU. These options control presentation and refresh cadence.
typedef struct {
    int64_t delayMS;
    bool showZombies;   // -z
    bool showIdle;      // -i  (show CPU idle tasks; hidden by default)
    bool adaptiveUnits; // -a
    bool noSummary;     // -s  (hide the cores/idle/system summary lines)
    bool logLedger;     // -l  (raw ledger to the system log each refresh —
                        //      the accounting's own checkout harness)
    bool perCore;       // -c  (one summary line per core — each core's books
                        //      against its own ledger interval)
    bool showThreads;   // -t  (expand multi-threaded tasks into per-thread
                        //      rows under their task; default is the honest
                        //      aggregate — Chris's ruling, 2026-08-07)
} top_options_t;

int32_t topMain(const top_options_t *opts);

#endif
