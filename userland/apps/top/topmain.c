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

typedef enum {
    TOP_SORT_CPU,
    TOP_SORT_TIME,
    TOP_SORT_TID,
    TOP_SORT_NAME,
    TOP_SORT_STATE,
    TOP_SORT_CORE,
    TOP_SORT_COUNT
} top_sort_t;

typedef struct {
    top_options_t options;
    top_sort_t sort;
    bool reverseSort;
    bool help;
    bool filterEditing;
    bool quit;
    char filter[64];
    char filterEdit[64];
    size_t filterEditLen;
} top_view_t;

static char ascii_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool contains_case_insensitive(const char *text, const char *needle)
{
    if (needle[0] == '\0')
        return true;

    for (size_t start = 0; text[start] != '\0'; start++)
    {
        size_t i = 0;
        while (needle[i] != '\0' && text[start + i] != '\0' &&
               ascii_lower(text[start + i]) == ascii_lower(needle[i]))
            i++;
        if (needle[i] == '\0')
            return true;
    }
    return false;
}

static int32_t string_compare_case_insensitive(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0')
    {
        char ac = ascii_lower(*a++);
        char bc = ascii_lower(*b++);
        if (ac != bc)
            return ac < bc ? -1 : 1;
    }
    if (*a == *b)
        return 0;
    return *a == '\0' ? -1 : 1;
}

static const char *sort_name(top_sort_t sort)
{
    static const char *names[] = {"CPU", "TIME", "TID", "NAME", "STATE", "CORE"};
    return names[sort];
}

static const char *sort_direction(const top_view_t *view)
{
    bool descending = view->sort == TOP_SORT_CPU || view->sort == TOP_SORT_TIME;
    if (view->reverseSort)
        descending = !descending;
    return descending ? "descending" : "ascending";
}

static bool handle_key(top_view_t *view, char c)
{
    if (view->filterEditing)
    {
        if (c == '\r' || c == '\n')
        {
            os64_strcopy(view->filter, sizeof(view->filter), view->filterEdit);
            view->filterEditing = false;
            return true;
        }
        if (c == 27) // Escape: abandon this edit, keep the applied filter
        {
            view->filterEditing = false;
            return true;
        }
        if (c == '\b' || c == 127)
        {
            if (view->filterEditLen > 0)
                view->filterEdit[--view->filterEditLen] = '\0';
            return true;
        }
        if (c >= ' ' && c <= '~' && view->filterEditLen + 1 < sizeof(view->filterEdit))
        {
            view->filterEdit[view->filterEditLen++] = c;
            view->filterEdit[view->filterEditLen] = '\0';
            return true;
        }
        return false;
    }

    switch (c)
    {
        case 'q': case 'Q': view->quit = true; return true;
        case 'h': case '?': view->help = !view->help; return true;
        case 's': view->sort = (top_sort_t)((view->sort + 1) % TOP_SORT_COUNT); return true;
        case 'S': view->reverseSort = !view->reverseSort; return true;
        case '/':
            os64_strcopy(view->filterEdit, sizeof(view->filterEdit), view->filter);
            view->filterEditLen = os64_strlen(view->filterEdit);
            view->filterEditing = true;
            return true;
        case 27: view->filter[0] = '\0'; return true;
        case 'z': case 'Z': view->options.showZombies = !view->options.showZombies; return true;
        case 'a': case 'A': view->options.adaptiveUnits = !view->options.adaptiveUnits; return true;
        case 'c': case 'C': view->options.perCore = !view->options.perCore; return true;
        default: return false;
    }
}

static bool poll_input(top_view_t *view)
{
    char input[16];
    int64_t n;
    bool changed = false;

    // Drain everything currently waiting. Zero-timeout means the display
    // loop never yields ownership of its clock to the keyboard.
    while ((n = os64_read_for(OS64_STDIN, input, sizeof(input), 0)) > 0)
        for (int64_t i = 0; i < n; i++)
            changed = handle_key(view, input[i]) || changed;

    return changed;
}

static bool wait_until_refresh(top_view_t *view, const os64_ticks_t *started,
                               uint64_t targetMS)
{
    uint64_t targetTicks = started->per_second
        ? (targetMS * started->per_second + 999) / 1000
        : 0;

    for (;;)
    {
        if (poll_input(view))
            return view->quit; // repaint immediately after any meaningful key

        os64_ticks_t now = {0};
        os64_ticks(&now);
        if (targetTicks == 0 || now.ticks < started->ticks ||
            now.ticks - started->ticks >= targetTicks)
            break;

        uint64_t remainingTicks = targetTicks - (now.ticks - started->ticks);
        uint64_t remainingMS = remainingTicks * 1000 / now.per_second;
        if (remainingMS == 0)
            remainingMS = 1;
        uint64_t slice = remainingMS > 50 ? 50 : remainingMS;
        os64_sleep(slice);
    }

    poll_input(view);
    return view->quit;
}

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

static void paint_frame(void)
{
    os64_write(OS64_STDOUT, "\f", 1);
    os64_write(OS64_STDOUT, frame, frameLen);
}

static void compose_help(const top_view_t *view)
{
    frame_reset();
    framef("os64 top - keys\n\n");
    framef("  q          leave\n");
    framef("  h  ?       toggle this help\n");
    framef("  s          next sort column\n");
    framef("  S          reverse sort direction\n");
    framef("  /          edit command filter\n");
    framef("  Esc        clear the active filter\n");
    framef("  z          show or hide zombies\n");
    framef("  a          fixed or adaptive time units\n");
    framef("  c          machine or per-core accounting\n\n");
    framef("  sort: %s %s   filter: %s\n",
           sort_name(view->sort), sort_direction(view),
           view->filter[0] ? view->filter : "(none)");
    framef("  zombies: %s   units: %s   cores: %s\n\n",
           view->options.showZombies ? "shown" : "hidden",
           view->options.adaptiveUnits ? "adaptive" : "fixed",
           view->options.perCore ? "per-core" : "machine");
    framef("  delay: %ldms   summary: %s   ledger log: %s\n\n",
           view->options.delayMS,
           view->options.noSummary ? "hidden" : "shown",
           view->options.logLedger ? "on" : "off");
    framef("The numbers are the kernel's books, not samples.\n");
    framef("CPU%% is one core; summaries describe the whole machine.\n\n");
    framef("Press h or ? to return.\n");
}

static void compose_footer(const top_view_t *view)
{
    if (view->filterEditing)
        framef("\nfilter: %s_   Enter apply  Esc cancel\n", view->filterEdit);
    else
        framef("\nq quit  ? help  s sort  S reverse  / filter  z zombies  a units  c cores\n");
}

// ── Small formatters ─────────────────────────────────────────────────────

// A percentage with one decimal, from a part and a whole, without floats:
// tenths = part*1000/whole, printed as t/10 "." t%10. Kept for the ONE
// reading whose whole signal lives below 1% — see tickskew's own note. The
// CPU splits use the integer apportionment below instead (ruled 2026-08-05:
// different instruments, different resolutions, each chosen for what it
// measures rather than for what Linux prints).
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

// ── Integer percentages that actually sum to 100 ─────────────────────────
// Turn `count` parts of `whole` into whole-number percentages whose total is
// EXACTLY 100. Rounding each one independently cannot do this: three parts
// truncated (or even rounded) on their own routinely total 99 or 101, and a
// summary line that visibly fails to add up is the exact complaint that
// started this — trading a jittery ±1% for a permanent −1% would have been
// no bargain.
//
// The fix is LARGEST REMAINDER: give everyone their floor, then hand the
// leftover units out to whoever was robbed most by the flooring. This is
// the apportionment problem — the same math as allocating seats in the US
// House of Representatives, which is where it was first argued (Hamilton's
// method, 1792, and Congress fought over the alternatives for a century).
// Dividing a fixed whole into whole-number shares that still total the
// whole turns out to be hard enough to have a political history; a CPU
// meter is a much smaller stage for it.
//
// CONTRACT: parts must sum to `whole` — the caller's arithmetic guarantees
// it (the kernel derives busy as total − idle − sched, so the three always
// close). The redistribution loop is bounded by `count` anyway, so a caller
// who breaks that contract gets a small bounded error rather than an
// inflated lie.
#define TOP_PCT_MAX 4

static void pct_apportion(const uint64_t *parts, uint32_t count,
                          uint64_t whole, uint32_t *out)
{
    uint64_t remainder[TOP_PCT_MAX];
    uint32_t assigned = 0;

    if (whole == 0 || count > TOP_PCT_MAX)
    {
        for (uint32_t i = 0; i < count; i++)
            out[i] = 0;
        return;
    }

    for (uint32_t i = 0; i < count; i++)
    {
        uint64_t scaled = parts[i] * 100;
        out[i] = (uint32_t)(scaled / whole);
        remainder[i] = scaled % whole;
        assigned += out[i];
    }

    // Flooring N values can lose at most N−1 whole units, so that bounds
    // this loop. Each entry may be rounded up once (its remainder is then
    // spent), which is what makes the result stable rather than lumpy.
    for (uint32_t pass = 0; assigned < 100 && pass < count; pass++)
    {
        uint32_t best = count;
        for (uint32_t i = 0; i < count; i++)
            if (remainder[i] > 0 && (best == count || remainder[i] > remainder[best]))
                best = i;
        if (best == count)
            break;   // every floor was exact; nothing is owed
        out[best]++;
        remainder[best] = 0;
        assigned++;
    }
}

// One percentage on its own, rounded half-up rather than truncated. For the
// per-task CPU column, where the rows are independent readings that never
// have to sum to anything — so there is nothing to apportion, only a digit
// to round honestly. A task burning a steady fraction of a percent rounds
// to 0 here and is meant to: the TIME column is the slow-leak detector,
// and it measures accumulated microseconds, which is strictly better at
// that job than an instantaneous percentage ever was.
static void fmt_pct_int(char *buf, size_t cap, uint64_t part, uint64_t whole)
{
    if (whole == 0)
    {
        os64_strcopy(buf, cap, "-");
        return;
    }
    os64_snprintf(buf, (int32_t)cap, "%lu", (part * 100 + whole / 2) / whole);
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

static int32_t compare_entries(const top_entry_t *a, const top_entry_t *b,
                               const top_view_t *view)
{
    uint64_t av = 0, bv = 0;
    int32_t result = 0;

    switch (view->sort)
    {
        case TOP_SORT_CPU:
            av = a->havePrev ? a->runtimeUS - a->prevRuntimeUS : 0;
            bv = b->havePrev ? b->runtimeUS - b->prevRuntimeUS : 0;
            result = av > bv ? -1 : av < bv ? 1 : 0;
            break;
        case TOP_SORT_TIME:
            result = a->runtimeUS > b->runtimeUS ? -1 :
                     a->runtimeUS < b->runtimeUS ? 1 : 0;
            break;
        case TOP_SORT_TID:
            result = a->TID < b->TID ? -1 : a->TID > b->TID ? 1 : 0;
            break;
        case TOP_SORT_NAME:
            result = string_compare_case_insensitive(a->Command, b->Command);
            break;
        case TOP_SORT_STATE:
            result = a->State < b->State ? -1 : a->State > b->State ? 1 : 0;
            break;
        case TOP_SORT_CORE:
            result = a->core < b->core ? -1 : a->core > b->core ? 1 : 0;
            break;
        default:
            break;
    }

    if (result != 0 && view->reverseSort)
        result = -result;
    if (result == 0)
        result = a->TID < b->TID ? -1 : a->TID > b->TID ? 1 : 0;
    return result;
}

static void sort_entries(top_entry_t **entries, uint32_t count,
                         const top_view_t *view)
{
    // Stable insertion sort is ideal for top's small, nearly sorted table.
    for (uint32_t i = 1; i < count; i++)
    {
        top_entry_t *key = entries[i];
        uint32_t j = i;
        while (j > 0 && compare_entries(entries[j - 1], key, view) > 0)
        {
            entries[j] = entries[j - 1];
            j--;
        }
        entries[j] = key;
    }
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
        else if (os64_streq(line, "core"))
            entry->core = (uint32_t)os64_atou(value);
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
    top_view_t view = {0};
    view.options = *opts;
    view.sort = TOP_SORT_CPU;

    os64_ticks(&tickThen);

    while (1 == 1)   // Ctrl+C is a fine interface for communicating with top
    {
        if (view.help)
        {
            os64_ticks_t helpStarted = {0};
            os64_ticks(&helpStarted);
            compose_help(&view);
            paint_frame();
            if (wait_until_refresh(&view, &helpStarted,
                                   (uint64_t)view.options.delayMS))
                break;
            continue;
        }

        os64_ticks_t refreshStarted = {0};
        os64_ticks(&refreshStarted);
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
                if (!view.options.showZombies)
                    continue;
            }
            if (!contains_case_insensitive(e->Command, view.filter))
                continue;
            if (shownCount < MAX_ENTRIES)
                shown[shownCount++] = e;
        }
        os64_close((int32_t)dirHandle);

        readCores();
        os64_ticks(&tickNow);

        // ── One clock to rule the division (Chris's ruling, 2026-07-30
        // small hours: "everything should be using the same clock").
        // Numerators (runtime_us, core columns) are LEDGER-clock (TSC→µs).
        // If the denominator comes from the TICK clock, every skew between
        // the two smears across every row — the P5's lockstep 100.5%s, the
        // VBox −45%. So the interval is measured on the LEDGER'S OWN CLOCK:
        // Δ(core 0's total_us) between refreshes. Core 0 always exists, and
        // settle-on-read guarantees its meter is fresh at every read.
        // The tick-clock interval is still computed — the DISAGREEMENT
        // between the two is promoted to a first-class display line (skew),
        // so tick-stream pathology becomes a watched number instead of a
        // smear. Percentages sum by construction; the truth stays visible.
        uint64_t tickIntervalUS = 0;
        if (tickNow.per_second > 0)
            tickIntervalUS = (tickNow.ticks - tickThen.ticks) * 1000000
                             / tickNow.per_second;
        tickThen = tickNow;

        // Recalibration guard must run BEFORE the ledger interval is taken
        // (re-priced history can step any column backward once a minute).
        for (int32_t c = 0; c < coreCount; c++)
        {
            if (coresNow[c].total < coresPrev[c].total) coresPrev[c].total = coresNow[c].total;
            if (coresNow[c].busy  < coresPrev[c].busy)  coresPrev[c].busy  = coresNow[c].busy;
            if (coresNow[c].idle  < coresPrev[c].idle)  coresPrev[c].idle  = coresNow[c].idle;
            if (coresNow[c].sched < coresPrev[c].sched) coresPrev[c].sched = coresNow[c].sched;
        }

        uint64_t ledgerIntervalUS = 0;
        if (haveCorePrev && coreCount > 0)
            ledgerIntervalUS = coresNow[0].total - coresPrev[0].total;

        // The ledger interval is the denominator when it's sane; the tick
        // interval covers the first refresh and recalibration blips (a
        // just-re-priced core 0 can post a near-zero delta for one round).
        uint64_t intervalUS = tickIntervalUS;
        if (ledgerIntervalUS > tickIntervalUS / 2 &&
            ledgerIntervalUS < tickIntervalUS * 4)
            intervalUS = ledgerIntervalUS;

        // A counter that went BACKWARD reads as zero delta, not as a
        // 584-million-year spike: TSC recalibration re-prices the whole
        // cycles→µs history, so a rate correction can step every
        // runtime_us back slightly. Unsigned subtraction would turn that
        // blip into the biggest number in the universe.
        for (uint32_t i = 0; i < shownCount; i++)
            if (shown[i]->runtimeUS < shown[i]->prevRuntimeUS)
                shown[i]->prevRuntimeUS = shown[i]->runtimeUS;

        sort_entries(shown, shownCount, &view);

        // ── The checkout channel (-l): raw ledger to the system log ──
        // Chris's protocol: emit what the accounting ACTUALLY said, every
        // refresh, in raw microseconds — no display rounding, no
        // percentages — so the serial log becomes a dataset and the
        // accounting can be audited statistically instead of by eyeball.
        // MUST run here, while prev counters still hold LAST refresh's
        // values (the display loop advances them). Cores first (the
        // machine's books), then every row top holds, deltas included.
        // One line per record, "toplog" prefix for grep.
        if (view.options.logLedger)
        {
            char lbuf[224];
            os64_snprintf(lbuf, sizeof(lbuf),
                          "toplog iter=%lu int_us=%lu cores=%d",
                          topIterationsExecuted, intervalUS, coreCount);
            os64_debug_log(lbuf);
            for (int32_t c = 0; c < coreCount; c++)
            {
                os64_snprintf(lbuf, sizeof(lbuf),
                              "toplog core=%d total=%lu busy=%lu idle=%lu sched=%lu dtotal=%lu didle=%lu",
                              c, coresNow[c].total, coresNow[c].busy,
                              coresNow[c].idle, coresNow[c].sched,
                              coresNow[c].total - coresPrev[c].total,
                              coresNow[c].idle - coresPrev[c].idle);
                os64_debug_log(lbuf);
            }
            for (uint32_t i = 0; i < shownCount; i++)
            {
                top_entry_t *e = shown[i];
                os64_snprintf(lbuf, sizeof(lbuf),
                              "toplog tid=%lu name=%s state=%s run_us=%lu d_us=%lu",
                              e->TID, e->Command, state_name(e->State),
                              e->runtimeUS,
                              e->havePrev ? e->runtimeUS - e->prevRuntimeUS : 0);
                os64_debug_log(lbuf);
            }
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
        framef("view: sort *%s %s   filter: %s%s%s\n",
               sort_name(view.sort), sort_direction(&view),
               view.filter[0] ? "\"" : "",
               view.filter[0] ? view.filter : "(none)",
               view.filter[0] ? "\"" : "");
        framef("      zombies %s   time %s   cores %s\n",
               view.options.showZombies ? "shown" : "hidden",
               view.options.adaptiveUnits ? "adaptive" : "fixed",
               view.options.perCore ? "per-core" : "machine");

        framef("tasks: %u shown, %u zombie%s%s, %u total\n",
               shownCount, zombies, zombies == 1 ? "" : "s",
               view.options.showZombies ? "" : " (hidden)", seen);

        if (!view.options.noSummary && haveCorePrev && intervalUS > 0)
        {
            // Machine-wide summary (Chris's ruling: the top of the screen
            // speaks in %-of-the-whole-machine). A core whose meter didn't
            // move is PARKED (BSPSCHED never woke it): it contributes its
            // whole interval as idle and is never divided by.
            uint64_t dBusy = 0, dIdle = 0, dSched = 0;
            int32_t parked = 0;
            for (int32_t c = 0; c < coreCount; c++)
            {
                // (Recalibration clamps already ran, before the ledger
                // interval was taken from core 0.)
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
            // Close the percentage against the ledger's own buckets. The
            // tick clock belongs only in tickskew below; using its interval
            // here made that disagreement leak directly into the CPU split.
            uint64_t machineUS = dBusy + dIdle + dSched;
            // Whole numbers, apportioned so the three ALWAYS total 100 —
            // the closure is now arithmetic rather than aspiration.
            uint64_t machineParts[3] = { dBusy, dIdle, dSched };
            uint32_t machinePct[3];
            pct_apportion(machineParts, 3, machineUS, machinePct);

            // The two clocks' disagreement, on its own leash: positive =
            // the tick clock claims MORE time passed than the TSC ledger
            // (lost-tick pathology reads negative — ticks fell behind).
            // On a healthy box this reads ±0.x%; on a stuttering P5 it
            // twitches with each hiccup; on VBox it IS the mystery.
            //
            // THE ONE READING THAT KEEPS ITS DECIMAL, and the reason the
            // rest gave theirs up: this instrument's entire useful range
            // lives below 1%. Round it to whole numbers and a healthy box
            // and a mildly sick one both read "+0%" — that is not a
            // simpler display, it is a deleted one. Magnitude through the
            // shared formatter; the sign is this reading's alone, because
            // it is the only percentage here that can be negative (ticks
            // falling BEHIND the TSC is a different pathology from ticks
            // running ahead, and the sign is which).
            char skewBuf[16];
            if (ledgerIntervalUS > 0)
            {
                int64_t diff = (int64_t)tickIntervalUS - (int64_t)ledgerIntervalUS;
                uint64_t mag = (uint64_t)(diff < 0 ? -diff : diff);
                char magBuf[16];
                fmt_pct(magBuf, sizeof(magBuf), mag, ledgerIntervalUS);
                os64_snprintf(skewBuf, sizeof(skewBuf), "%c%s",
                              diff < 0 ? '-' : '+', magBuf);
            }
            else
                os64_strcopy(skewBuf, sizeof(skewBuf), "-");
            framef("cores: %d (%d parked)   busy %u%%   idle %u%%   sched %u%%   tickskew %s%%\n",
                   coreCount, parked, machinePct[0], machinePct[1], machinePct[2],
                   skewBuf);

            // -c: each core's books against ITS OWN ledger interval — the
            // per-core closure test. busy is kernel-derived (total − idle −
            // sched) so each line sums to 100.0 BY IDENTITY; the value is in
            // the split, and in comparing a core's busy% against the task
            // rows wearing its number in the C column.
            if (view.options.perCore)
            {
                for (int32_t c = 0; c < coreCount; c++)
                {
                    uint64_t dT = coresNow[c].total - coresPrev[c].total;
                    if (dT < intervalUS / 100)
                    {
                        framef("  core %2d: parked\n", c);
                        continue;
                    }
                    uint64_t coreParts[3] = {
                        coresNow[c].busy  - coresPrev[c].busy,
                        coresNow[c].idle  - coresPrev[c].idle,
                        coresNow[c].sched - coresPrev[c].sched,
                    };
                    uint32_t corePct[3];
                    pct_apportion(coreParts, 3, dT, corePct);
                    framef("  core %2d: busy %u%%   idle %u%%   sched %u%%   (%lums)\n",
                           c, corePct[0], corePct[1], corePct[2], dT / 1000);
                }
            }
        }
        else if (!view.options.noSummary)
        {
            framef("cores: %d   (first interval - measuring)\n", coreCount);
        }

        framef("\n%-6s %-9s %1s %2s %6s %10s  %s\n",
               view.sort == TOP_SORT_TID ? "*TID" : "TID",
               view.sort == TOP_SORT_STATE ? "*STATE" : "STATE",
               "K", view.sort == TOP_SORT_CORE ? "*C" : "C",
               view.sort == TOP_SORT_CPU ? "*CPU%" : "CPU%",
               view.sort == TOP_SORT_TIME ? "*TIME" : "TIME",
               view.sort == TOP_SORT_NAME ? "*COMMAND" : "COMMAND");

        for (uint32_t i = 0; i < shownCount; i++)
        {
            top_entry_t *e = shown[i];
            uint64_t dUS = e->havePrev ? e->runtimeUS - e->prevRuntimeUS : 0;

            // Per-task CPU% is of ONE CPU (the other half of the ruling):
            // 100.0 means "ate a whole core", however many cores exist.
            char pctBuf[16], timeBuf[24];
            if (e->havePrev && intervalUS > 0)
                fmt_pct_int(pctBuf, sizeof(pctBuf), dUS, intervalUS);
            else
                os64_strcopy(pctBuf, sizeof(pctBuf), "-");
            fmt_time(timeBuf, sizeof(timeBuf), e->runtimeUS, view.options.adaptiveUnits);

            framef("%-6lu %-9s %1s %2u %6s %10s  %s\n",
                   e->TID, state_name(e->State),
                   e->KernelProc ? "k" : " ",
                   e->core, pctBuf, timeBuf, e->Command);

            e->prevRuntimeUS = e->runtimeUS;
            e->havePrev = true;
        }

        // Advance every task sampled this round, including rows hidden by the
        // zombie or command filters. Revealing one later must not charge all
        // of its hidden runtime to a single refresh.
        for (uint32_t i = 0; i < topEntryCount; i++)
        {
            top_entry_t *e = &top_entries[i];
            if (e->lastIterationUsed == topIterationsExecuted)
            {
                e->prevRuntimeUS = e->runtimeUS;
                e->havePrev = true;
            }
        }

        for (int32_t c = 0; c < coreCount; c++)
            coresPrev[c] = coresNow[c];
        haveCorePrev = true;

        compose_footer(&view);

        // ── Paint: one clear, one write ───────────────────────────────
        paint_frame();

        if (wait_until_refresh(&view, &refreshStarted,
                               (uint64_t)view.options.delayMS))
            break;
    }
    return 0;
}
