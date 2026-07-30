// topmain.c — the engine behind top. Chris designed the instrument (the
// display rulings, the stat-file contract, the zombie and units knobs);
// the library guy — back from "playing" with the network, thanks for the
// heckling in the old temporaryAtoi comments — wound it.
//
// The measurement doctrine (PROC.md "cores"): runtime_us in each status
// file and the /proc/cores ledger are BOUNDARY-CHARGED microsecond
// counters. top never computes time itself — it snapshots the counters,
// waits, snapshots again, and divides deltas. The interval is measured
// with os64_ticks (never assumed from the delay: sleep rounds up, screens
// take time to paint, and a denominator that lies makes every row lie).

#include "topmain.h"

#define MAX_ENTRIES 512
#define MAX_CORES 32
#define STATUS_LINE_SIZE 256
#define FRAME_SIZE 16384

static const char procDir[] = "/proc";

static top_entry_t top_entries[MAX_ENTRIES];
static uint64_t topIterationsExecuted = 0;

// One core's ledger row, twice (this refresh and the last one).
typedef struct {
    uint64_t total, busy, idle, sched;
} core_sample_t;
static core_sample_t coresNow[MAX_CORES], coresPrev[MAX_CORES];
static int32_t coreCount = 0;
static bool haveCorePrev = false;

// ── The frame ────────────────────────────────────────────────────────────
// The whole screen is composed into one buffer and written in ONE call
// after the \f — a repaint should be a single write, not a drizzle of
// lines racing the console (and someday, when the escape-sequence slice
// gives us cursor addressing, this buffer is what gets diffed).
static char frame[FRAME_SIZE];
static size_t frameLen = 0;

static void frame_reset(void)
{
    frameLen = 0;
    frame[0] = '\0';
}

static void framef(const char *fmt, ...)
{
    if (frameLen >= FRAME_SIZE - 1)
        return;   // full frame: drop quietly, the write stays bounded
    va_list args;
    va_start(args, fmt);
    int32_t n = os64_vsnprintf(frame + frameLen, FRAME_SIZE - frameLen, fmt, args);
    va_end(args);
    if (n > 0)
    {
        frameLen += (size_t)n;
        if (frameLen > FRAME_SIZE - 1)
            frameLen = FRAME_SIZE - 1;
    }
}

// ── Small formatters ─────────────────────────────────────────────────────

// A percentage with one decimal, from a part and a whole, without floats:
// tenths = part*1000/whole, printed as t/10 "." t%10.
static void fmt_pct(char *buf, size_t cap, uint64_t part, uint64_t whole)
{
    if (whole == 0)
    {
        os64_strcopy(buf, cap, "-");
        return;
    }
    uint64_t tenths = part * 1000 / whole;
    os64_snprintf(buf, (int32_t)cap, "%lu.%lu", tenths / 10, tenths % 10);
}

// CPU time for the TIME column. Default: X.Y seconds, always — scannable
// at a glance at a 1-second cadence (Chris's ruling; adaptive units are
// "cool, I'll use it, but sometimes distracting"). With -a: 500us / 12ms /
// 1.6s, so mayflies read in their natural unit.
static void fmt_time(char *buf, size_t cap, uint64_t us, bool adaptive)
{
    if (adaptive && us < 1000)
        os64_snprintf(buf, (int32_t)cap, "%luus", us);
    else if (adaptive && us < 1000000)
        os64_snprintf(buf, (int32_t)cap, "%lums", us / 1000);
    else
        os64_snprintf(buf, (int32_t)cap, "%lu.%lus",
                      us / 1000000, (us % 1000000) / 100000);
}

static const char *state_name(eTaskState s)
{
    switch (s)
    {
        case THREAD_STATE_RUNNING:  return "running";
        case THREAD_STATE_RUNNABLE: return "runnable";
        case THREAD_STATE_STOPPED:  return "stopped";
        case THREAD_STATE_USLEEP:   return "usleep";
        case THREAD_STATE_ISLEEP:   return "isleep";
        case THREAD_STATE_ZOMBIE:   return "zombie";
        default:                    return "?";
    }
}

static eTaskState state_from_name(const char *state)
{
    if (os64_streq(state, "running"))  return THREAD_STATE_RUNNING;
    if (os64_streq(state, "runnable")) return THREAD_STATE_RUNNABLE;
    if (os64_streq(state, "stopped"))  return THREAD_STATE_STOPPED;
    if (os64_streq(state, "usleep"))   return THREAD_STATE_USLEEP;
    if (os64_streq(state, "isleep"))   return THREAD_STATE_ISLEEP;
    if (os64_streq(state, "zombie"))   return THREAD_STATE_ZOMBIE;
    return THREAD_STATE_NONE;
}

// ── /proc parsing ────────────────────────────────────────────────────────

// One status file → one entry. key\tvalue per line (the /proc format
// contract — Plan 9's tradition, PROC.md's law).
static int32_t parseStatusFile(int32_t fileHandle, top_entry_t *entry)
{
    char line[STATUS_LINE_SIZE];
    int64_t readResult;

    while ((readResult = os64_readline(fileHandle, line, sizeof(line))) == 1)
    {
        char *value = line;
        while (*value != '\0' && *value != ' ' && *value != '\t')
            value++;
        if (*value == '\0')
            continue;
        *value++ = '\0';
        while (*value == ' ' || *value == '\t')
            value++;

        if (os64_streq(line, "task"))
            entry->TID = os64_atou(value);
        else if (os64_streq(line, "name"))
            os64_strcopy(entry->Command, sizeof(entry->Command), value);
        else if (os64_streq(line, "state"))
            entry->State = state_from_name(value);
        else if (os64_streq(line, "parent"))
            entry->PTID = os64_atou(value);
        else if (os64_streq(line, "kernel"))
            entry->KernelProc = os64_streq(value, "yes");
        else if (os64_streq(line, "runtime_us"))
            entry->runtimeUS = os64_atou(value);
    }

    return readResult < 0 ? (int32_t)readResult : 0;
}

// /proc/cores → coresNow[]. First line is the header (starts with "core");
// data rows are tab-separated: id total busy idle sched, all µs.
static int32_t readCores(void)
{
    char line[STATUS_LINE_SIZE];
    int64_t h = os64_open("/proc/cores", NULL);
    if (h < 0)
        return -1;

    coreCount = 0;
    while (os64_readline((int32_t)h, line, sizeof(line)) == 1 &&
           coreCount < MAX_CORES)
    {
        if (line[0] < '0' || line[0] > '9')
            continue;   // the header row

        const char *p = line;
        uint64_t v[5] = {0};
        for (int32_t f = 0; f < 5; f++)
        {
            while (*p != '\0' && (*p < '0' || *p > '9'))
                p++;
            v[f] = os64_atou(p);
            while (*p >= '0' && *p <= '9')
                p++;
        }
        // v[0] is the core id; rows arrive in order, but trust the id.
        if (v[0] < MAX_CORES)
        {
            coresNow[v[0]].total = v[1];
            coresNow[v[0]].busy  = v[2];
            coresNow[v[0]].idle  = v[3];
            coresNow[v[0]].sched = v[4];
            if ((int32_t)v[0] + 1 > coreCount)
                coreCount = (int32_t)v[0] + 1;
        }
    }
    os64_close((int32_t)h);
    return 0;
}

// ── Entry cache ──────────────────────────────────────────────────────────
// Find the entry for a TID, or claim a slot: exact match first, then a
// stale slot (its task vanished — reuse resets the delta history so a
// recycled slot can't inherit a dead task's runtime), then a fresh one.
static int64_t getTopEntryToUse(uint64_t tid, top_entry_t **out,
                                uint32_t *topEntryCount)
{
    top_entry_t *stale = NULL;

    *out = NULL;
    for (uint32_t idx = 0; idx < *topEntryCount; idx++)
    {
        if (top_entries[idx].TID == tid)
        {
            *out = &top_entries[idx];
            (*out)->lastIterationUsed = topIterationsExecuted;
            return 0;
        }
        if (stale == NULL &&
            top_entries[idx].lastIterationUsed + 2 <= topIterationsExecuted)
            stale = &top_entries[idx];
    }

    if (stale != NULL)
    {
        os64_memset(stale, 0, sizeof(*stale));
        stale->TID = tid;
        stale->lastIterationUsed = topIterationsExecuted;
        *out = stale;
        return 0;
    }

    if (*topEntryCount < MAX_ENTRIES)
    {
        *out = &top_entries[*topEntryCount];
        os64_memset(*out, 0, sizeof(**out));
        (*out)->TID = tid;
        (*out)->lastIterationUsed = topIterationsExecuted;
        (*topEntryCount)++;
        return 0;
    }
    return TOP_ERROR_NO_FREE_TOP_ENTRIES;
}

// ── The main loop ────────────────────────────────────────────────────────

int32_t topMain(const top_options_t *opts)
{
    uint32_t topEntryCount = 0;
    os64_ticks_t tickThen = {0}, tickNow = {0};
    os64_dirent_t dent;

    os64_ticks(&tickThen);

    while (1 == 1)   // Ctrl+C is a fine interface for communicating with top
    {
        topIterationsExecuted++;

        // ── Gather ───────────────────────────────────────────────────
        int64_t dirHandle = os64_opendir(procDir);
        if (dirHandle < 0)
        {
            os64_hprintf(OS64_STDERR, "top: cannot open %s\n", procDir);
            return TOP_ERROR_CANNOT_READ_PROC_DIRECTORY;
        }

        uint32_t seen = 0, zombies = 0;
        top_entry_t *shown[MAX_ENTRIES];
        uint32_t shownCount = 0;

        while (os64_readdir((int32_t)dirHandle, &dent) == 1)
        {
            if (dent.name[0] < '0' || dent.name[0] > '9')
                continue;   // "cores" and any future non-task names

            char path[64];
            os64_snprintf(path, sizeof(path), "/proc/%s/status", dent.name);
            int64_t sh = os64_open(path, NULL);
            if (sh < 0)
                continue;   // the task died between readdir and open — fine

            top_entry_t *e = NULL;
            if (getTopEntryToUse(os64_atou(dent.name), &e, &topEntryCount) != 0)
            {
                os64_close((int32_t)sh);
                continue;   // table full: count what we can, drop the rest
            }

            e->runtimeUS = 0;
            parseStatusFile((int32_t)sh, e);
            os64_close((int32_t)sh);

            seen++;
            if (e->State == THREAD_STATE_ZOMBIE)
            {
                zombies++;
                if (!opts->showZombies)
                    continue;
            }
            if (shownCount < MAX_ENTRIES)
                shown[shownCount++] = e;
        }
        os64_close((int32_t)dirHandle);

        readCores();
        os64_ticks(&tickNow);

        // The measured interval, in µs — the one true denominator.
        uint64_t intervalUS = 0;
        if (tickNow.per_second > 0)
            intervalUS = (tickNow.ticks - tickThen.ticks) * 1000000
                         / tickNow.per_second;
        tickThen = tickNow;

        // A counter that went BACKWARD reads as zero delta, not as a
        // 584-million-year spike: TSC recalibration re-prices the whole
        // cycles→µs history, so a rate correction can step every
        // runtime_us back slightly. Unsigned subtraction would turn that
        // blip into the biggest number in the universe.
        for (uint32_t i = 0; i < shownCount; i++)
            if (shown[i]->runtimeUS < shown[i]->prevRuntimeUS)
                shown[i]->prevRuntimeUS = shown[i]->runtimeUS;

        // ── Sort shown rows by CPU delta, descending (it IS top) ─────
        for (uint32_t i = 1; i < shownCount; i++)
        {
            top_entry_t *key = shown[i];
            uint64_t kd = key->havePrev ? key->runtimeUS - key->prevRuntimeUS : 0;
            uint32_t j = i;
            while (j > 0)
            {
                top_entry_t *p = shown[j - 1];
                uint64_t pd = p->havePrev ? p->runtimeUS - p->prevRuntimeUS : 0;
                if (pd > kd || (pd == kd && p->TID <= key->TID))
                    break;
                shown[j] = p;
                j--;
            }
            shown[j] = key;
        }

        // ── Compose the frame ─────────────────────────────────────────
        frame_reset();

        os64_date_t now;
        if (os64_date_now(&now, NULL) == 0)
            framef("os64 top - %02d:%02d:%02d   ", now.hour, now.minute, now.second);
        else
            framef("os64 top   ");

        char upBuf[24];
        uint64_t upUS = (tickNow.per_second > 0)
                        ? tickNow.ticks * 1000000 / tickNow.per_second : 0;
        fmt_time(upBuf, sizeof(upBuf), upUS, false);
        framef("up %s   interval %lums   iter %lu\n",
               upBuf, intervalUS / 1000, topIterationsExecuted);

        framef("tasks: %u shown, %u zombie%s%s, %u total\n",
               shownCount, zombies, zombies == 1 ? "" : "s",
               opts->showZombies ? "" : " (hidden)", seen);

        if (!opts->noSummary && haveCorePrev && intervalUS > 0)
        {
            // Machine-wide summary (Chris's ruling: the top of the screen
            // speaks in %-of-the-whole-machine). A core whose meter didn't
            // move is PARKED (BSPSCHED never woke it): it contributes its
            // whole interval as idle and is never divided by.
            uint64_t dBusy = 0, dIdle = 0, dSched = 0;
            int32_t parked = 0;
            for (int32_t c = 0; c < coreCount; c++)
            {
                // Same recalibration guard as the task rows: re-priced
                // history can step any column backward once a minute.
                if (coresNow[c].total < coresPrev[c].total) coresPrev[c].total = coresNow[c].total;
                if (coresNow[c].busy  < coresPrev[c].busy)  coresPrev[c].busy  = coresNow[c].busy;
                if (coresNow[c].idle  < coresPrev[c].idle)  coresPrev[c].idle  = coresNow[c].idle;
                if (coresNow[c].sched < coresPrev[c].sched) coresPrev[c].sched = coresNow[c].sched;

                uint64_t dTotal = coresNow[c].total - coresPrev[c].total;
                if (dTotal < intervalUS / 100)
                {
                    parked++;
                    dIdle += intervalUS;
                    continue;
                }
                dBusy  += coresNow[c].busy  - coresPrev[c].busy;
                dIdle  += coresNow[c].idle  - coresPrev[c].idle;
                dSched += coresNow[c].sched - coresPrev[c].sched;
            }
            uint64_t machineUS = (uint64_t)coreCount * intervalUS;
            char b[16], i[16], s[16];
            fmt_pct(b, sizeof(b), dBusy, machineUS);
            fmt_pct(i, sizeof(i), dIdle, machineUS);
            fmt_pct(s, sizeof(s), dSched, machineUS);
            framef("cores: %d (%d parked)   busy %s%%   idle %s%%   sched %s%%\n",
                   coreCount, parked, b, i, s);
        }
        else if (!opts->noSummary)
        {
            framef("cores: %d   (first interval - measuring)\n", coreCount);
        }

        framef("\n%-6s %-9s %1s %6s %10s  %s\n",
               "TID", "STATE", "K", "CPU%", "TIME", "COMMAND");

        for (uint32_t i = 0; i < shownCount; i++)
        {
            top_entry_t *e = shown[i];
            uint64_t dUS = e->havePrev ? e->runtimeUS - e->prevRuntimeUS : 0;

            // Per-task CPU% is of ONE CPU (the other half of the ruling):
            // 100.0 means "ate a whole core", however many cores exist.
            char pctBuf[16], timeBuf[24];
            if (e->havePrev && intervalUS > 0)
                fmt_pct(pctBuf, sizeof(pctBuf), dUS, intervalUS);
            else
                os64_strcopy(pctBuf, sizeof(pctBuf), "-");
            fmt_time(timeBuf, sizeof(timeBuf), e->runtimeUS, opts->adaptiveUnits);

            framef("%-6lu %-9s %1s %6s %10s  %s\n",
                   e->TID, state_name(e->State),
                   e->KernelProc ? "k" : " ",
                   pctBuf, timeBuf, e->Command);

            e->prevRuntimeUS = e->runtimeUS;
            e->havePrev = true;
        }

        // Zombies still tracked even when hidden: their prev must advance
        // so un-hiding with a later -z run doesn't show phantom deltas.
        // (They're dead; their delta is zero anyway. Belt and suspenders.)

        for (int32_t c = 0; c < coreCount; c++)
            coresPrev[c] = coresNow[c];
        haveCorePrev = true;

        // ── Paint: one clear, one write ───────────────────────────────
        os64_write(OS64_STDOUT, "\f", 1);
        os64_write(OS64_STDOUT, frame, frameLen);

        os64_sleep((uint64_t)opts->delayMS);
    }
    return 0;
}
