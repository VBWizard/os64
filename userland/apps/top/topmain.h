#ifndef TOPMAIN_H
#define TOPMAIN_H
#include "os64/os64.h"

#define TOP_ERROR_CANNOT_STAT_PROC 1
#define TOP_ERROR_CANNOT_READ_PROC_DIRECTORY 2
#define TOP_ERROR_NO_FREE_TOP_ENTRIES 3
#define TOP_ERROR_CANNOT_READ_STATUS_FILE 4
#define TOP_ERROR_IN_GETTOPENTRYTOUSE 5

typedef enum {
    THREAD_STATE_NONE,
    THREAD_STATE_RUNNING,
    THREAD_STATE_RUNNABLE,
    THREAD_STATE_STOPPED,
    THREAD_STATE_USLEEP,
    THREAD_STATE_ISLEEP,
    THREAD_STATE_ZOMBIE
} eTaskState;

typedef struct topent {
    uint64_t TID, PTID;
    char Command[64];
    eTaskState State;
    bool KernelProc;
    // CPU time (runtime_us from the status file — the boundary-charged
    // truth, not the sampled `ticks`). prev is last refresh's reading; the
    // delta over the measured interval is the CPU% column.
    uint64_t runtimeUS;
    uint64_t prevRuntimeUS;
    bool havePrev;              // first sighting has no delta — show 0.0
    uint64_t lastIterationUsed; // stale entries (task vanished) get reused
} top_entry_t;

// The knobs, all ruled on by Chris (2026-07-29): summary percentages are
// of the WHOLE MACHINE, per-task rows are of ONE CPU; zombies hidden by
// default (no task cleanup yet — the morgue is standing room only);
// fixed X.Y-seconds TIME column by default, adaptive units opt-in;
// idle/system summary shown by default with an off switch.
typedef struct {
    int64_t delayMS;
    bool showZombies;   // -z
    bool adaptiveUnits; // -a
    bool noSummary;     // -s  (hide the cores/idle/system summary lines)
    bool logLedger;     // -l  (raw ledger to the system log each refresh —
                        //      the accounting's own checkout harness)
} top_options_t;

int32_t topMain(const top_options_t *opts);

#endif
