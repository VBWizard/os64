#include "htop.h"

static const char *const styles[HT_STYLE_COUNT] = {
    "\033[0;37;40m", "\033[0;90;40m", "\033[0;96;40m", "\033[0;92;40m",
    "\033[0;93;40m", "\033[0;91;40m", "\033[0;30;46m", "\033[0;30;46m", "\033[0;97;44m"
};
const char *ht_style(unsigned style) { return styles[style < HT_STYLE_COUNT ? style : HT_NORMAL]; }
static void cell(const ht_view_t *v, ht_cell_t *g, unsigned y, unsigned x, unsigned style, unsigned char c)
{
    if (y < v->lines && x < v->cols) g[y * HT_WIDTH + x] = (ht_cell_t){c, (unsigned char)style};
}
static void text(const ht_view_t *v, ht_cell_t *g, unsigned y, unsigned x, unsigned style, const char *s)
{
    // Process strings are data. C0/DEL/high bytes cannot become terminal
    // commands, line breaks or charset-dependent glyphs in the output stream.
    for (; *s && x < v->cols; s++, x++)
        cell(v, g, y, x, style, (unsigned char)*s >= 32 && (unsigned char)*s <= 126 ? (unsigned char)*s : '.');
}
static void print(const ht_view_t *v, ht_cell_t *g, unsigned y, unsigned x, unsigned style, const char *fmt, ...)
{
    char buf[512]; va_list ap; va_start(ap, fmt); os64_vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    text(v, g, y, x, style, buf);
}
static void band(const ht_view_t *v, ht_cell_t *g, unsigned y, unsigned style)
{ for (unsigned x = 0; x < v->cols; x++) cell(v, g, y, x, style, ' '); }
static void field(const ht_view_t *v, ht_cell_t *g, unsigned y, unsigned x, unsigned width,
                  unsigned style, const char *s)
{
    size_t n = os64_strlen(s);
    if (n > width) {
        for (unsigned i = 0; i < width; i++) cell(v, g, y, x + i, style, i + 1 == width ? '~' : (unsigned char)s[i]);
    } else text(v, g, y, x + width - (unsigned)n, style, s);
}
static void size_text(char *out, size_t cap, uint64_t bytes)
{
    const char units[] = "BKMGTPE"; unsigned unit = 0; uint64_t divisor = 1;
    while (unit < 6 && bytes / divisor >= 1024) { divisor *= 1024; unit++; }
    if (!unit) os64_snprintf(out, cap, "%luB", bytes);
    else os64_snprintf(out, cap, "%lu.%lu%c", bytes / divisor, (bytes % divisor) * 10 / divisor, units[unit]);
}
static void time_text(char *out, size_t cap, uint64_t us)
{ os64_snprintf(out, cap, "%lu:%02lu.%lu", us / 60000000, us / 1000000 % 60, us / 100000 % 10); }
static unsigned filled(uint64_t part, uint64_t whole, unsigned width)
{
    if (!whole) return 0;
    if (part >= whole) return width;
    unsigned n = 0;
    for (unsigned i = 1; i <= width; i++) {
        uint64_t threshold = (whole / width) * i + ((whole % width) * i + width - 1) / width;
        if (part < threshold) break;
        n = i;
    }
    return n;
}
static void meter(const ht_view_t *v, ht_cell_t *g, unsigned y, unsigned x, unsigned width,
                  uint64_t part, uint64_t extra, uint64_t whole, bool valid)
{
    if (width < 3) return;
    cell(v, g, y, x, HT_DIM, '['); cell(v, g, y, x + width - 1, HT_DIM, ']');
    unsigned a = valid ? filled(part, whole, width - 2) : 0;
    unsigned b = valid ? filled(part + extra, whole, width - 2) : 0;
    for (unsigned i = 0; i < width - 2; i++)
        cell(v, g, y, x + i + 1, i < a ? HT_GREEN : i < b ? HT_AMBER : HT_DIM,
             !valid ? '-' : i < b ? '|' : '.');
}
static const char *sort_name(ht_sort_t sort)
{
    static const char *names[] = {"CPU", "HEAP", "TIME", "PID", "NAME"};
    return names[sort];
}
static void title(const ht_view_t *v, ht_cell_t *g, const char *label)
{
    band(v, g, 0, HT_TITLE);
    print(v, g, 0, 2, HT_TITLE, "htop / os64   %s", label);
    if (v->sample && v->cols >= 60) {
        uint64_t up = v->sample->ticks.per_second ? v->sample->ticks.ticks / v->sample->ticks.per_second : 0;
        print(v, g, 0, v->cols - 25, HT_TITLE, "up %lud %02lu:%02lu:%02lu", up / 86400, up / 3600 % 24, up / 60 % 60, up % 60);
    }
}
static void header(ht_view_t *v, ht_cell_t *g)
{
    const os64_monitor_snapshot_t *s = v->sample;
    char live[32]; os64_snprintf(live, sizeof(live), "%s  %ums", v->paused ? "PAUSED" : "LIVE", v->delay_ms);
    title(v, g, live);
    if (!s) { text(v, g, 2, 2, HT_AMBER, "Waiting for a system sample..."); return; }
    unsigned half = v->cols / 2, mw = half > 18 ? half - 18 : 3;
    text(v, g, 1, 2, HT_CYAN, "CPU");
    meter(v, g, 1, 6, mw, s->machine_percent[0], s->machine_percent[2], 100, s->summary_valid);
    if (s->summary_valid) print(v, g, 1, 7 + mw, HT_GREEN, "%3u%%", s->machine_percent[0] + s->machine_percent[2]);
    else text(v, g, 1, 7 + mw, HT_DIM, " -- ");
    print(v, g, 2, 2, HT_DIM, "busy %u%%  sched %u%%", s->machine_percent[0], s->machine_percent[2]);
    if (!s->summary_valid) text(v, g, 2, 2, HT_DIM, "measuring / unavailable");
    text(v, g, 1, half + 1, HT_CYAN, "MEM");
    meter(v, g, 1, half + 5, mw, s->memory.used, 0, s->memory.usable, s->memory_valid);
    if (s->memory_valid) {
        char used[24], total[24]; size_text(used, sizeof(used), s->memory.used); size_text(total, sizeof(total), s->memory.usable);
        print(v, g, 2, half + 1, HT_DIM, "%s / %s usable", used, total);
        print(v, g, 1, half + 6 + mw, HT_GREEN, "%3u%%", filled(s->memory.used, s->memory.usable, 100));
    } else text(v, g, 2, half + 1, HT_DIM, "memory unavailable");
    unsigned core_rows = v->table_top - 6, per_page = core_rows * 2;
    if (per_page && v->core_page * per_page >= s->core_count) v->core_page = 0;
    for (unsigned j = 0; j < per_page && v->core_page * per_page + j < s->core_count; j++) {
        const os64_monitor_core_t *c = &s->cores[v->core_page * per_page + j];
        unsigned x = j % 2 ? half + 1 : 2, y = 3 + j / 2;
        print(v, g, y, x, HT_DIM, "%3u", c->id);
        meter(v, g, y, x + 4, mw, c->percent[0], c->percent[2], 100, c->valid);
        if (!c->valid) text(v, g, y, x + mw + 5, HT_DIM, " -- ");
        else if (c->parked) text(v, g, y, x + mw + 5, HT_DIM, "park");
        else print(v, g, y, x + mw + 5, HT_NORMAL, "%3u%%", c->percent[0] + c->percent[2]);
    }
    unsigned y = 3 + core_rows;
    print(v, g, y, 2, HT_CYAN, "%lu tasks  %lu zombies", (uint64_t)s->task_count, (uint64_t)s->zombies);
    char available[24]; size_text(available, sizeof(available), s->memory.available);
    if (s->memory_valid) print(v, g, y, half + 1, HT_NORMAL, "%s available", available);
    text(v, g, y + 1, 2, HT_DIM, "activity");
    unsigned trail = half > 12 ? half - 12 : 0;
    if (trail > HT_HISTORY) trail = HT_HISTORY;
    static const char levels[] = " .:-=+*#%@";
    for (unsigned i = 0; i < trail; i++) {
        unsigned back = trail - i;
        if (back > v->history_count) continue;
        unsigned value = v->history[(v->history_next + HT_HISTORY - back) % HT_HISTORY];
        cell(v, g, y + 1, 11 + i, value == 255 ? HT_DIM : HT_CYAN, value == 255 ? '?' : levels[value * 9 / 100]);
    }
    print(v, g, y + 1, half + 1, HT_DIM, "cores %u-%u/%lu  [ ] page", s->core_count ? v->core_page * per_page + 1 : 0,
          (unsigned)(s->core_count < (v->core_page + 1) * per_page ? s->core_count : (v->core_page + 1) * per_page), (uint64_t)s->core_count);
}
static void task_rows(const ht_view_t *v, ht_cell_t *g)
{
    band(v, g, v->table_top - 1, HT_COLUMN);
    unsigned heading = v->table_top - 1;
    field(v, g, heading, 1, 5, HT_COLUMN, "ID");
    text(v, g, heading, 7, HT_COLUMN, "S");
    field(v, g, heading, 9, 3, HT_COLUMN, "C");
    field(v, g, heading, 13, 5, HT_COLUMN, "CPU%");
    field(v, g, heading, 19, 8, HT_COLUMN, "HEAP");
    field(v, g, heading, 28, 9, HT_COLUMN, "TIME");
    text(v, g, heading, 39, HT_COLUMN, "COMMAND");
    for (unsigned line = 0; line < v->page; line++) {
        size_t at = v->scroll + line; if (at >= v->count) break;
        const ht_row_t *r = &v->rows[at];
        const os64_monitor_task_t *t = &v->sample->tasks[r->task];
        const os64_thread_info_t *th = r->is_thread ? &v->sample->threads[r->thread].info : NULL;
        const os64_monitor_usage_t *cpu = r->is_thread ? &v->sample->threads[r->thread].cpu : &t->cpu;
        bool selected = at == v->selected; unsigned style = selected ? HT_SELECTED : HT_NORMAL;
        unsigned y = v->table_top + line;
        if (selected) band(v, g, y, HT_SELECTED);
        char buf[32]; os64_snprintf(buf, sizeof(buf), "%lu", th ? th->tid : t->info.pid); field(v, g, y, 1, 5, style, buf);
        cell(v, g, y, 7, style, (unsigned char)os64_proc_state_letter(th ? th->state : t->info.state));
        os64_snprintf(buf, sizeof(buf), "%u", th ? th->core : t->info.core); field(v, g, y, 9, 3, style, buf);
        if (cpu->valid) os64_snprintf(buf, sizeof(buf), "%lu", cpu->percent); else os64_strcopy(buf, sizeof(buf), "-");
        field(v, g, y, 13, 5, selected ? style : cpu->valid && cpu->percent ? HT_GREEN : HT_DIM, buf);
        if (!th && t->info.heap) size_text(buf, sizeof(buf), t->info.heap); else os64_strcopy(buf, sizeof(buf), "-");
        field(v, g, y, 19, 8, style, buf);
        time_text(buf, sizeof(buf), th ? th->runtime_us : t->info.runtime_us); field(v, g, y, 28, 9, style, buf);
        unsigned x = 39;
        if (r->depth) {
            unsigned indent = r->depth > 6 ? 6 : r->depth;
            x += indent * 2; text(v, g, y, x - 2, selected ? style : HT_DIM, r->is_thread ? ": " : "+-");
        }
        const char *name = th ? t->info.name : t->info.command;
        if (!*name) name = t->info.name;
        size_t n = os64_strlen(name), offset = v->command_offset < n ? v->command_offset : n;
        text(v, g, y, x, selected ? style : t->info.kernel || th ? HT_DIM : HT_NORMAL, name + offset);
    }
    if (!v->count) text(v, g, v->table_top + 1, 2, HT_DIM, "No tasks match this view. Esc clears the filter.");
}
static void footer(const ht_view_t *v, ht_cell_t *g)
{
    unsigned y = v->lines - 2;
    if (v->mode == HT_FILTER) print(v, g, y, 1, HT_AMBER, "Filter: %s_   Enter apply / Esc cancel", v->edit);
    else if (v->notice[0]) text(v, g, y, 1, HT_AMBER, v->notice);
    else if (v->sample && v->sample->partial) print(v, g, y, 1, HT_AMBER, "PARTIAL SAMPLE: %s%s%s",
          v->sample->partial & OS64_MONITOR_TASKS_PARTIAL ? "tasks " : "",
          v->sample->partial & OS64_MONITOR_THREADS_PARTIAL ? "threads " : "",
          v->sample->partial & OS64_MONITOR_CORES_PARTIAL ? "cores " : "");
    else print(v, g, y, 1, HT_DIM, "%lu/%lu  %s%s  %s  threads:%s idle:%s zombies:%s  /%s",
          (uint64_t)(v->count ? v->selected + 1 : 0), (uint64_t)v->count,
          sort_name(v->sort), v->reverse ? " rev" : "", v->tree ? "tree" : "list",
          v->threads ? "on" : "off", v->idle ? "on" : "off", v->zombies ? "on" : "off", v->filter);
    band(v, g, y + 1, HT_COLUMN);
    text(v, g, y + 1, 1, HT_COLUMN, "q Quit  ? Help  / Filter  s Sort  t Threads  v Tree  Enter Info  x Signal");
}
static void help(const ht_view_t *v, ht_cell_t *g)
{
    title(v, g, "KEYS");
    const char *lines[] = {
        "Up/Down or j/k   Select a row (selection follows its ID)",
        "PgUp/PgDn        Page through tasks",
        "Home/End         First / last row",
        "Left/Right       Scroll the command horizontally",
        "/ then Enter     Filter names and commands; Esc clears",
        "s / S            Next sort column / reverse order",
        "v / t            Process tree / expand thread rows",
        "i / z            Show CPU idle tasks / zombies",
        "Enter            Task details; arrows scroll the command",
        "x / X            Interrupt / kill the selected TASK",
        "                 Both ask first; thread rows name their owner",
        "Space pause/resume; + faster / - slower refresh",
        "[ / ]            Previous / next bank of CPU meters",
        "Green = busy; amber = scheduler. CPU% uses one core.",
        "HEAP excludes code/stacks; '-' is zero or unreported.",
        "q exits. In a panel, q or Esc returns to the list."
    };
    for (unsigned i = 0; i < sizeof(lines) / sizeof(lines[0]) && i + 2 < v->lines - 1; i++)
        text(v, g, i + 2, 2, i < 13 ? HT_NORMAL : HT_DIM, lines[i]);
    band(v, g, v->lines - 1, HT_COLUMN); text(v, g, v->lines - 1, 2, HT_COLUMN, "? / Esc  Back to the monitor");
}
static void details(ht_view_t *v, ht_cell_t *g)
{
    title(v, g, "TASK DETAILS");
    const os64_monitor_task_t *t = ht_selected_task(v); if (!t) return;
    const os64_proc_info_t *p = &t->info; char heap[24], runtime[32];
    size_text(heap, sizeof(heap), p->heap); time_text(runtime, sizeof(runtime), p->runtime_us);
    print(v, g, 2, 2, HT_CYAN, "%s  /  task %lu", p->name, p->pid);
    print(v, g, 4, 2, HT_NORMAL, "Parent %lu   %s   core %u   %u threads", p->ppid, os64_proc_state_name(p->state), p->core, p->threads);
    print(v, g, 5, 2, HT_NORMAL, "Time %s   heap %s   %s task", runtime, p->heap ? heap : "unreported / zero", p->kernel ? "kernel" : "user");
    print(v, g, 6, 2, HT_NORMAL, "Faults %lu minor / %lu major   switches %lu", p->minor_faults, p->major_faults, p->switches);
    text(v, g, 8, 2, HT_CYAN, v->command ? "Command" : "Command summary (full report unavailable)");
    const char *command = v->command ? v->command : p->command;
    size_t length = os64_strlen(command); unsigned width = v->cols > 4 ? v->cols - 4 : 1;
    unsigned room = v->lines > 12 ? v->lines - 12 : 1;
    size_t command_lines = length / width + (length % width != 0);
    size_t max_line = command_lines > room ? command_lines - room : 0;
    if (v->detail_line > max_line) v->detail_line = (unsigned)max_line;
    size_t start = (size_t)v->detail_line * width;
    for (unsigned line = 0; line < room && start < length; line++) {
        for (unsigned col = 0; col < width && start < length; col++, start++) {
            unsigned char c = (unsigned char)command[start];
            cell(v, g, 9 + line, 2 + col, HT_NORMAL, c >= 32 && c <= 126 ? c : '.');
        }
    }
    text(v, g, v->lines - 2, 2, HT_DIM, "Heap is allocator-reported memory, not total process memory.");
    band(v, g, v->lines - 1, HT_COLUMN);
    text(v, g, v->lines - 1, 2, HT_COLUMN, "Esc Back   Up/Down / PgUp/PgDn Scroll command");
}
static void confirm(const ht_view_t *v, ht_cell_t *g)
{
    title(v, g, v->action_kill ? "KILL TASK" : "INTERRUPT TASK");
    print(v, g, 3, 3, v->action_kill ? HT_RED : HT_AMBER, "%s task %lu?", v->action_kill ? "Kill" : "Interrupt", v->action_pid);
    text(v, g, 5, 3, HT_NORMAL, v->action_name);
    text(v, g, 7, 3, HT_DIM, v->action_kill ? "Ends this task and its threads immediately." : "Sends an interrupt to this task, including its threads.");
    text(v, g, 8, 3, HT_DIM, v->action_kill ? "The task cannot decline this signal." : "The task may handle or exit on the interrupt.");
    text(v, g, 11, 3, HT_CYAN, "y Confirm    n / Esc Cancel");
    text(v, g, v->lines - 2, 3, HT_DIM, "The target stays pinned while the process list changes.");
}
void ht_draw(ht_view_t *v, ht_cell_t *g)
{
    for (unsigned y = 0; y < HT_HEIGHT; y++) for (unsigned x = 0; x < HT_WIDTH; x++)
        g[y * HT_WIDTH + x] = (ht_cell_t){' ', HT_NORMAL};
    if (v->cols < 59 || v->lines < 18) {
        text(v, g, 0, 0, HT_CYAN, "htop / os64");
        text(v, g, 2, 0, HT_AMBER, "Please resize to at least 60 x 18.");
        text(v, g, 4, 0, HT_NORMAL, v->paused ? "q quits. Sampling is paused." : "q quits. Sampling continues."); return;
    }
    if (v->mode == HT_HELP) help(v, g);
    else if (v->mode == HT_DETAILS) details(v, g);
    else if (v->mode == HT_CONFIRM) confirm(v, g);
    else { header(v, g); task_rows(v, g); footer(v, g); }
}
