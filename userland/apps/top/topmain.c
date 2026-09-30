// top presents snapshots from libos64; sampling history and CPU accounting
// live in os64/monitor.h so terminal and graphical consumers share them.

#include "topmain.h"
#include "os64/monitor.h"

#define MAX_ENTRIES 512
#define MAX_CORES 32
#define FRAME_SIZE 16384

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

// Match the kernel's per-core idle tasks, not sleeping user processes.
static bool idle_task(const os64_proc_info_t *task)
{
    if (!task->kernel || os64_memcmp(task->name, "idle", 4) || !task->name[4])
        return false;
    for (const char *c = task->name + 4; *c; c++)
        if (*c < '0' || *c > '9')
            return false;
    return true;
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
        case 'i': case 'I': view->options.showIdle = !view->options.showIdle; return true;
        case 'z': case 'Z': view->options.showZombies = !view->options.showZombies; return true;
        case 'a': case 'A': view->options.adaptiveUnits = !view->options.adaptiveUnits; return true;
        case 'c': case 'C': view->options.perCore = !view->options.perCore; return true;
        case 't': case 'T': view->options.showThreads = !view->options.showThreads; return true;
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

// ── The frame ────────────────────────────────────────────────────────────
// Compose a complete frame before writing it to the terminal.
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

static void compose_free_memory(uint64_t freeBytes)
{
    const uint64_t mb = 1024 * 1024;
    const uint64_t gb = 1024 * mb;

    if (freeBytes >= gb * 32)
    {
        uint64_t whole = freeBytes / gb;
        uint64_t tenths = ((freeBytes % gb) * 10) / gb;
        framef("free %lu.%lu GB   ", whole, tenths);
    }
    else
        framef("free %lu MB   ", freeBytes / mb);
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
    framef("  i          show or hide CPU idle tasks\n");
    framef("  z          show or hide zombies\n");
    framef("  a          fixed or adaptive time units\n");
    framef("  c          machine or per-core accounting\n");
    framef("  t          expand multi-threaded tasks into thread rows\n\n");
    framef("  sort: %s %s   filter: %s\n",
           sort_name(view->sort), sort_direction(view),
           view->filter[0] ? view->filter : "(none)");
    framef("  idle tasks: %s   zombies: %s   units: %s   cores: %s\n\n",
           view->options.showIdle ? "shown" : "hidden",
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
        framef("\nq quit  ? help  s/S sort  / filter  i idle  z zombies  a units  c cores  t threads\n");
}

// ── Small formatters ─────────────────────────────────────────────────────

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

static int32_t compare_entries(const os64_monitor_task_t *a, const os64_monitor_task_t *b,
                               const top_view_t *view)
{
    uint64_t av = 0, bv = 0;
    int32_t result = 0;

    switch (view->sort)
    {
        case TOP_SORT_CPU:
            av = a->cpu.delta_us;
            bv = b->cpu.delta_us;
            result = av > bv ? -1 : av < bv ? 1 : 0;
            break;
        case TOP_SORT_TIME:
            result = a->info.runtime_us > b->info.runtime_us ? -1 :
                     a->info.runtime_us < b->info.runtime_us ? 1 : 0;
            break;
        case TOP_SORT_TID:
            result = a->info.pid < b->info.pid ? -1 : a->info.pid > b->info.pid ? 1 : 0;
            break;
        case TOP_SORT_NAME:
            result = string_compare_case_insensitive(a->info.name, b->info.name);
            break;
        case TOP_SORT_STATE:
            result = a->info.state < b->info.state ? -1 : a->info.state > b->info.state ? 1 : 0;
            break;
        case TOP_SORT_CORE:
            result = a->info.core < b->info.core ? -1 : a->info.core > b->info.core ? 1 : 0;
            break;
        default:
            break;
    }

    if (result != 0 && view->reverseSort)
        result = -result;
    if (result == 0)
        result = a->info.pid < b->info.pid ? -1 : a->info.pid > b->info.pid ? 1 : 0;
    return result;
}

static void sort_entries(const os64_monitor_task_t **entries, uint32_t count,
                         const top_view_t *view)
{
    // Stable insertion sort is ideal for top's small, nearly sorted table.
    for (uint32_t i = 1; i < count; i++)
    {
        const os64_monitor_task_t *key = entries[i];
        uint32_t j = i;
        while (j > 0 && compare_entries(entries[j - 1], key, view) > 0)
        {
            entries[j] = entries[j - 1];
            j--;
        }
        entries[j] = key;
    }
}

int32_t topMain(const top_options_t *opts)
{
    os64_monitor_t *monitor = os64_monitor_create(MAX_ENTRIES, MAX_ENTRIES, MAX_CORES);
    if (!monitor) {
        os64_hprintf(OS64_STDERR, "top: cannot allocate monitor\n");
        return 3;
    }
    top_view_t view = {0};
    view.options = *opts;
    view.sort = TOP_SORT_CPU;
    int32_t result = 0;

    for (;;)
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
        const os64_monitor_snapshot_t *sample;
        if (os64_monitor_sample(monitor, view.options.showThreads, &sample) < 0) {
            os64_hprintf(OS64_STDERR, "top: cannot sample processes or clock\n");
            result = 2;
            break;
        }
        const os64_monitor_task_t *shown[MAX_ENTRIES];
        uint32_t shownCount = 0;
        for (size_t i = 0; i < sample->task_count; i++) {
            const os64_monitor_task_t *task = &sample->tasks[i];
            if (!view.options.showIdle && idle_task(&task->info))
                continue;
            if (!view.options.showZombies && task->info.state == OS64_PROC_ZOMBIE)
                continue;
            if (contains_case_insensitive(task->info.name, view.filter))
                shown[shownCount++] = task;
        }
        uint64_t intervalUS = sample->interval_us;
        int32_t coreCount = (int32_t)sample->core_count;

        sort_entries(shown, shownCount, &view);

        // Keep the raw ledger log alongside top's display. Reading a snapshot
        // does not advance its counters; the next sample owns that transition.
        if (view.options.logLedger)
        {
            char lbuf[224];
            os64_snprintf(lbuf, sizeof(lbuf),
                          "toplog iter=%lu int_us=%lu cores=%d",
                          sample->sequence, intervalUS, coreCount);
            os64_debug_log(lbuf);
            for (int32_t c = 0; c < coreCount; c++)
            {
                os64_snprintf(lbuf, sizeof(lbuf),
                              "toplog core=%u total=%lu busy=%lu idle=%lu sched=%lu dtotal=%lu didle=%lu",
                              sample->cores[c].id, sample->cores[c].time.total, sample->cores[c].time.busy,
                              sample->cores[c].time.idle, sample->cores[c].time.sched,
                              sample->cores[c].delta.total, sample->cores[c].delta.idle);
                os64_debug_log(lbuf);
            }
            for (uint32_t i = 0; i < shownCount; i++)
            {
                const os64_monitor_task_t *e = shown[i];
                os64_snprintf(lbuf, sizeof(lbuf),
                              "toplog tid=%lu name=%s state=%s run_us=%lu d_us=%lu",
                              e->info.pid, e->info.name, os64_proc_state_name(e->info.state),
                              e->info.runtime_us,
                              e->cpu.delta_us);
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

        if (sample->memory_valid)
            compose_free_memory(sample->memory.free);
        else
            framef("free n/a   ");

        char upBuf[24];
        uint64_t upUS = (sample->ticks.per_second > 0)
                        ? sample->ticks.ticks * 1000000 / sample->ticks.per_second : 0;
        fmt_time(upBuf, sizeof(upBuf), upUS, false);
        framef("up %s   interval %lums   iter %lu\n",
               upBuf, intervalUS / 1000, sample->sequence);
        framef("view: sort *%s %s   filter: %s%s%s\n",
               sort_name(view.sort), sort_direction(&view),
               view.filter[0] ? "\"" : "",
               view.filter[0] ? view.filter : "(none)",
               view.filter[0] ? "\"" : "");
        framef("      idle tasks %s   zombies %s   time %s   cores %s\n",
               view.options.showIdle ? "shown" : "hidden",
               view.options.showZombies ? "shown" : "hidden",
               view.options.adaptiveUnits ? "adaptive" : "fixed",
               view.options.perCore ? "per-core" : "machine");

        framef("tasks: %u shown, %u zombie%s%s, %u total\n",
               shownCount, (uint32_t)sample->zombies, sample->zombies == 1 ? "" : "s",
               view.options.showZombies ? "" : " (hidden)", (uint32_t)sample->task_count);
        if (sample->partial)
            framef("sample incomplete: tasks %s  threads %s  cores %s\n",
                   sample->partial & OS64_MONITOR_TASKS_PARTIAL ? "partial" : "ok",
                   sample->partial & OS64_MONITOR_THREADS_PARTIAL ? "partial" : "ok",
                   sample->partial & OS64_MONITOR_CORES_PARTIAL ? "partial" : "ok");

        if (!view.options.noSummary && sample->summary_valid)
        {
            char skewBuf[32];
            if (sample->skew_valid)
                os64_snprintf(skewBuf, sizeof(skewBuf), "%c%lu.%lu",
                              sample->skew_negative ? '-' : '+',
                              sample->skew_tenths / 10, sample->skew_tenths % 10);
            else
                os64_strcopy(skewBuf, sizeof(skewBuf), "-");
            framef("cores: %d (%d parked)   busy %u%%   idle %u%%   sched %u%%   tickskew %s%%\n",
                   coreCount, (int32_t)sample->parked, sample->machine_percent[0],
                   sample->machine_percent[1], sample->machine_percent[2], skewBuf);
            if (view.options.perCore)
            {
                for (size_t c = 0; c < sample->core_count; c++)
                {
                    const os64_monitor_core_t *core = &sample->cores[c];
                    if (core->parked) {
                        framef("  core %2u: parked\n", core->id);
                        continue;
                    }
                    framef("  core %2u: busy %u%%   idle %u%%   sched %u%%   (%lums)\n",
                           core->id, core->percent[0], core->percent[1], core->percent[2],
                           core->delta.total / 1000);
                }
            }
        }
        else if (!view.options.noSummary)
        {
            framef("cores: %d   (interval unavailable - measuring)\n", coreCount);
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
            const os64_monitor_task_t *e = shown[i];

            // Per-task CPU% is of ONE CPU (the other half of the ruling):
            // 100.0 means "ate a whole core", however many cores exist.
            char pctBuf[16], timeBuf[24];
            if (e->cpu.valid && intervalUS > 0)
                os64_snprintf(pctBuf, sizeof(pctBuf), "%lu", e->cpu.percent);
            else
                os64_strcopy(pctBuf, sizeof(pctBuf), "-");
            fmt_time(timeBuf, sizeof(timeBuf), e->info.runtime_us, view.options.adaptiveUnits);

            framef("%-6lu %-9s %1s %2u %6s %10s  %s\n",
                   e->info.pid, os64_proc_state_name(e->info.state),
                   e->info.kernel ? "k" : " ",
                   e->info.core, pctBuf, timeBuf, e->info.name);

            // Attach sampled thread rows to their owning task for display.
            for (size_t t = 0; t < sample->thread_count; t++)
            {
                const os64_monitor_thread_t *tr = &sample->threads[t];
                if (tr->info.pid != e->info.pid)
                    continue;

                char tPct[16], tTime[24];
                if (tr->cpu.valid && intervalUS > 0)
                    os64_snprintf(tPct, sizeof(tPct), "%lu", tr->cpu.percent);
                else
                    os64_strcopy(tPct, sizeof(tPct), "-");
                fmt_time(tTime, sizeof(tTime), tr->info.runtime_us,
                         view.options.adaptiveUnits);

                framef("%-6lu %-9s %1s %2u %6s %10s   - %s\n",
                       tr->info.tid, os64_proc_state_name(tr->info.state),
                       e->info.kernel ? "k" : " ",
                       tr->info.core, tPct, tTime, e->info.name);

            }
        }

        compose_footer(&view);

        // ── Paint: one clear, one write ───────────────────────────────
        paint_frame();

        if (wait_until_refresh(&view, &refreshStarted,
                               (uint64_t)view.options.delayMS))
            break;
    }
    os64_monitor_destroy(monitor);
    return result;
}
