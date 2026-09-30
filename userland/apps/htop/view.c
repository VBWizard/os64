#include "htop.h"

static char lower(char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; }
static bool contains(const char *s, const char *needle)
{
    if (!*needle) return true;
    for (; *s; s++) {
        size_t i = 0;
        while (needle[i] && s[i] && lower(s[i]) == lower(needle[i])) i++;
        if (!needle[i]) return true;
    }
    return false;
}
static bool idle_task(const os64_proc_info_t *p)
{
    if (!p->kernel || os64_memcmp(p->name, "idle", 4) || !p->name[4]) return false;
    for (const char *c = p->name + 4; *c; c++) if (*c < '0' || *c > '9') return false;
    return true;
}
static bool same_identity(ht_identity_t a, ht_identity_t b)
{
    return a.valid && b.valid && a.pid == b.pid && a.is_thread == b.is_thread &&
           (!a.is_thread || a.tid == b.tid);
}
static ht_identity_t row_identity(const ht_view_t *v, size_t row)
{
    ht_identity_t id = {0};
    if (row >= v->count) return id;
    const ht_row_t *r = &v->rows[row];
    id.valid = true; id.pid = v->sample->tasks[r->task].info.pid; id.is_thread = r->is_thread;
    if (r->is_thread) id.tid = v->sample->threads[r->thread].info.tid;
    return id;
}
const os64_monitor_task_t *ht_selected_task(const ht_view_t *v)
{
    return v->sample && v->selected < v->count ? &v->sample->tasks[v->rows[v->selected].task] : NULL;
}
static void keep_visible(ht_view_t *v)
{
    if (!v->count) { v->selected = v->scroll = 0; return; }
    if (v->selected >= v->count) v->selected = v->count - 1;
    if (v->scroll > v->selected) v->scroll = v->selected;
    if (v->page && v->selected >= v->scroll + v->page) v->scroll = v->selected - v->page + 1;
    size_t max_scroll = v->count > v->page ? v->count - v->page : 0;
    if (v->scroll > max_scroll) v->scroll = max_scroll;
}
void ht_init(ht_view_t *v, uint64_t self)
{
    os64_memset(v, 0, sizeof(*v)); v->self = self; v->sort = HT_CPU; v->delay_ms = 1000;
    ht_layout(v, 80, 25);
}
void ht_layout(ht_view_t *v, unsigned cols, unsigned lines)
{
    v->cols = cols > HT_WIDTH ? HT_WIDTH : cols;
    if (v->cols) v->cols--; // Leave the physical rightmost cell alone to avoid wrap.
    v->lines = lines > HT_HEIGHT ? HT_HEIGHT : lines;
    unsigned core_rows = v->sample ? (unsigned)((v->sample->core_count + 1) / 2) : 0;
    if (core_rows > 4) core_rows = 4;
    if (lines < 24 && core_rows > 2) core_rows = 2;
    v->table_top = 6 + core_rows;
    v->page = v->lines > v->table_top + 3 ? v->lines - v->table_top - 3 : 1;
    keep_visible(v);
}
static int compare(const ht_view_t *v, uint16_t ai, uint16_t bi)
{
    const os64_monitor_task_t *a = &v->sample->tasks[ai], *b = &v->sample->tasks[bi];
    uint64_t av = 0, bv = 0; int result = 0;
    switch (v->sort) {
    case HT_CPU: av = a->cpu.delta_us; bv = b->cpu.delta_us; break;
    case HT_HEAP: av = a->info.heap; bv = b->info.heap; break;
    case HT_TIME: av = a->info.runtime_us; bv = b->info.runtime_us; break;
    case HT_PID: av = b->info.pid; bv = a->info.pid; break;
    case HT_NAME: {
        const char *x = a->info.name, *y = b->info.name;
        while (*x && *y && lower(*x) == lower(*y)) { x++; y++; }
        result = lower(*x) < lower(*y) ? -1 : lower(*x) > lower(*y); break;
    }
    default: break;
    }
    if (v->sort != HT_NAME) result = av > bv ? -1 : av < bv;
    if (v->reverse) result = -result;
    if (!result) result = a->info.pid < b->info.pid ? -1 : a->info.pid > b->info.pid;
    return result;
}
static void append_task(ht_view_t *v, uint16_t i, unsigned depth)
{
    if (v->count == HT_ROWS) return;
    v->rows[v->count++] = (ht_row_t){.task = i, .depth = (uint16_t)depth};
    if (!v->threads) return;
    for (size_t t = 0; t < v->sample->thread_count && t < HT_THREADS && v->count < HT_ROWS; t++)
        if (v->sample->threads[t].info.pid == v->sample->tasks[i].info.pid)
            v->rows[v->count++] = (ht_row_t){i, (uint16_t)t, (uint16_t)(depth + 1), true};
}
void ht_rebuild(ht_view_t *v, const os64_monitor_snapshot_t *s, bool history)
{
    uint16_t sorted[HT_TASKS], stack[HT_TASKS], depths[HT_TASKS];
    int16_t parent[HT_TASKS]; bool visited[HT_TASKS] = {0}; size_t count = 0;
    v->sample = s; v->count = 0;
    if (!s) { v->identity.valid = false; if (v->mode == HT_CONFIRM || v->mode == HT_DETAILS) v->mode = HT_LIST; return; }
    for (size_t i = 0; i < s->task_count && i < HT_TASKS; i++) {
        const os64_proc_info_t *p = &s->tasks[i].info;
        if ((!v->idle && idle_task(p)) || (!v->zombies && p->state == OS64_PROC_ZOMBIE)) continue;
        if (!contains(p->name, v->filter) && !contains(p->command, v->filter)) continue;
        size_t j = count;
        while (j && compare(v, sorted[j - 1], (uint16_t)i) > 0) { sorted[j] = sorted[j - 1]; j--; }
        sorted[j] = (uint16_t)i; count++;
    }
    if (!v->tree) {
        for (size_t i = 0; i < count; i++) append_task(v, sorted[i], 0);
    } else {
        // Only visible parents participate. Orphans become roots; a visited
        // set lets malformed/cyclic parent chains remain visible without recursion.
        for (size_t i = 0; i < count; i++) {
            parent[i] = -1;
            for (size_t j = 0; j < count; j++)
                if (i != j && s->tasks[sorted[i]].info.ppid == s->tasks[sorted[j]].info.pid)
                    { parent[i] = (int16_t)j; break; }
        }
        for (unsigned pass = 0; pass < 2; pass++) for (size_t root = 0; root < count; root++) {
            if (visited[root] || (!pass && parent[root] >= 0)) continue;
            size_t used = 1; stack[0] = (uint16_t)root; depths[0] = 0;
            while (used) {
                unsigned depth = depths[--used]; uint16_t at = stack[used];
                if (visited[at]) continue;
                visited[at] = true; append_task(v, sorted[at], depth);
                for (size_t j = count; j > 0; j--)
                    if (!visited[j - 1] && parent[j - 1] == (int16_t)at && used < HT_TASKS) {
                        stack[used] = (uint16_t)(j - 1); depths[used++] = (uint16_t)(depth + 1);
                    }
            }
        }
    }
    bool found = false;
    for (size_t i = 0; i < v->count; i++) if (same_identity(v->identity, row_identity(v, i))) {
        v->selected = i; found = true; break;
    }
    if (!found && v->mode == HT_DETAILS) {
        v->mode = HT_LIST; os64_strcopy(v->notice, sizeof(v->notice), "Selected task ended or left this view.");
    }
    keep_visible(v); v->identity = row_identity(v, v->selected);
    if (v->mode == HT_CONFIRM) {
        found = false;
        for (size_t i = 0; i < s->task_count; i++) {
            const os64_proc_info_t *p = &s->tasks[i].info;
            if (p->pid == v->action_pid && p->state != OS64_PROC_ZOMBIE &&
                !p->kernel && os64_streq(p->name, v->action_name)) found = true;
        }
        if (!found) { v->mode = HT_LIST; os64_strcopy(v->notice, sizeof(v->notice), "Target ended or changed; no signal sent."); }
    }
    if (history) {
        v->history[v->history_next] = s->summary_valid ? (uint8_t)(s->machine_percent[0] + s->machine_percent[2]) : 255;
        v->history_next = (v->history_next + 1) % HT_HISTORY;
        if (v->history_count < HT_HISTORY) v->history_count++;
    }
    unsigned actual_cols = v->cols + 1;
    ht_layout(v, actual_cols, v->lines);
}
static void select_move(ht_view_t *v, int amount)
{
    if (amount < 0) v->selected = v->selected < (size_t)-amount ? 0 : v->selected - (size_t)-amount;
    else if (v->count) v->selected = v->selected + (size_t)amount >= v->count ? v->count - 1 : v->selected + (size_t)amount;
    keep_visible(v); v->identity = row_identity(v, v->selected);
}
int ht_key(ht_view_t *v, int key)
{
    if (key == 3 || key == 4 || (key == 'q' && (v->cols < 59 || v->lines < 18))) { v->quit = true; return 0; }
    if (v->mode == HT_CONFIRM) {
        if (key == 'y' && v->confirm_drawn) { v->mode = HT_LIST; return HT_SEND_SIGNAL; }
        if (key == 27 || key == 'n' || key == 'q') v->mode = HT_LIST;
        return 0;
    }
    if (v->mode == HT_FILTER) {
        if (key == 27) v->mode = HT_LIST;
        else if (key == '\r' || key == '\n') {
            os64_strcopy(v->filter, sizeof(v->filter), v->edit); v->mode = HT_LIST;
            ht_rebuild(v, v->sample, false);
        } else if (key == 8 || key == 127) {
            if (v->edit_len) v->edit[--v->edit_len] = 0;
        } else if (key >= 32 && key <= 126 && v->edit_len + 1 < sizeof(v->edit)) {
            v->edit[v->edit_len++] = (char)key; v->edit[v->edit_len] = 0;
        }
        return 0;
    }
    if (v->mode == HT_HELP) { if (key == '?' || key == 'h' || key == 27 || key == 'q') v->mode = HT_LIST; return 0; }
    if (v->mode == HT_DETAILS) {
        if (key == 27 || key == 'q' || key == '\r' || key == '\n') v->mode = HT_LIST;
        else if (key == HT_DOWN || key == 'j') v->detail_line++;
        else if ((key == HT_UP || key == 'k') && v->detail_line) v->detail_line--;
        else if (key == HT_PGDN) v->detail_line += v->page;
        else if (key == HT_PGUP) v->detail_line = v->detail_line > v->page ? v->detail_line - v->page : 0;
        else if (key == HT_HOME) v->detail_line = 0;
        return 0;
    }
    v->notice[0] = 0;
    switch (key) {
    case 'q': v->quit = true; break;
    case '?': case 'h': v->mode = HT_HELP; break;
    case '/':
        v->mode = HT_FILTER; os64_strcopy(v->edit, sizeof(v->edit), v->filter); v->edit_len = os64_strlen(v->edit); break;
    case 27: v->filter[0] = 0; ht_rebuild(v, v->sample, false); break;
    case HT_UP: case 'k': select_move(v, -1); break;
    case HT_DOWN: case 'j': select_move(v, 1); break;
    case HT_PGUP: select_move(v, -(int)v->page); break;
    case HT_PGDN: select_move(v, (int)v->page); break;
    case HT_HOME: v->selected = 0; select_move(v, 0); break;
    case HT_END: v->selected = v->count ? v->count - 1 : 0; select_move(v, 0); break;
    case HT_LEFT: v->command_offset = v->command_offset > 8 ? v->command_offset - 8 : 0; break;
    case HT_RIGHT: if (v->command_offset < OS64_PROC_COMMAND_MAX - 8) v->command_offset += 8; break;
    case 's': v->sort = (ht_sort_t)((v->sort + 1) % HT_SORT_COUNT); v->reverse = false; ht_rebuild(v, v->sample, false); break;
    case 'S': v->reverse = !v->reverse; ht_rebuild(v, v->sample, false); break;
    case 'v': v->tree = !v->tree; ht_rebuild(v, v->sample, false); break;
    case 't': v->threads = !v->threads; ht_rebuild(v, v->sample, false); return HT_REFRESH_THREADS;
    case 'i': v->idle = !v->idle; ht_rebuild(v, v->sample, false); break;
    case 'z': v->zombies = !v->zombies; ht_rebuild(v, v->sample, false); break;
    case ' ': v->paused = !v->paused; break;
    case '[': if (v->core_page) v->core_page--; break;
    case ']': if (v->sample && v->table_top > 6 && (v->core_page + 1) * (v->table_top - 6) * 2 < v->sample->core_count) v->core_page++; break;
    case '+': v->delay_ms = v->delay_ms / 2 < 100 ? 100 : v->delay_ms / 2; break;
    case '-': v->delay_ms = v->delay_ms * 2 > 60000 ? 60000 : v->delay_ms * 2; break;
    case '\r': case '\n':
        if (v->count) { v->mode = HT_DETAILS; v->detail_line = 0; return HT_LOAD_DETAILS; } break;
    case 'x': case 'X': {
        const os64_monitor_task_t *t = ht_selected_task(v);
        if (!t) break;
        if (t->info.kernel || t->info.pid == v->self || t->info.state == OS64_PROC_ZOMBIE) {
            os64_strcopy(v->notice, sizeof(v->notice), "Choose a live user task other than this monitor."); break;
        }
        v->action_pid = t->info.pid; v->action_kill = key == 'X'; v->confirm_drawn = false;
        os64_strcopy(v->action_name, sizeof(v->action_name), t->info.name); v->mode = HT_CONFIRM; break;
    }
    default: break;
    }
    return 0;
}

int ht_decode(ht_input_t *in, unsigned char c)
{
    if (!in->state) {
        if (c == 27) { in->state = 1; in->length = in->number = 0; return 0; }
        return c;
    }
    if (in->state == 1) {
        if (c == '[' || c == 'O') { in->state = 2; return 0; }
        in->state = 0; in->pending = c; return 27;
    }
    if (++in->length > 24) in->state = 3;
    if (c >= 0x40 && c <= 0x7e) {
        bool discard = in->state == 3; in->state = 0;
        if (discard) return 0;
        switch (c) {
        case 'A': return HT_UP; case 'B': return HT_DOWN;
        case 'C': return HT_RIGHT; case 'D': return HT_LEFT;
        case 'H': return HT_HOME; case 'F': return HT_END;
        case 'P': return '?'; case 'R': return '/';
        case '~':
            switch (in->number) {
            case 1: case 7: return HT_HOME; case 4: case 8: return HT_END;
            case 5: return HT_PGUP; case 6: return HT_PGDN;
            case 11: return '?'; case 13: return '/'; case 15: return 'v';
            case 17: return 's'; case 20: return 'x'; case 21: return 'q';
            default: return 0;
            }
        default: return 0;
        }
    }
    if (c >= '0' && c <= '9' && in->number < 1000) in->number = in->number * 10 + c - '0';
    else if (c != ';') in->state = 3;
    return 0;
}
int ht_escape_timeout(ht_input_t *in)
{
    int key = in->state == 1 ? 27 : 0; in->state = 0; return key;
}

int ht_send_signal(const ht_view_t *v)
{
    os64_proc_info_t current;
    if (!v->action_pid || v->action_pid == v->self ||
        os64_proc_read(v->action_pid, &current) < 0 || current.kernel ||
        current.state == OS64_PROC_ZOMBIE || current.state == OS64_PROC_NONE ||
        !os64_streq(current.name, v->action_name)) return -1;
    char path[64]; os64_snprintf(path, sizeof(path), "/proc/%lu/ctl", v->action_pid);
    int64_t h = os64_open(path, "w"); if (h < 0) return -1;
    const char *verb = v->action_kill ? "kill" : "interrupt";
    int64_t n = os64_write((int32_t)h, verb, os64_strlen(verb));
    int64_t closed = os64_close((int32_t)h);
    return n == (int64_t)os64_strlen(verb) && !closed ? 0 : -1;
}
