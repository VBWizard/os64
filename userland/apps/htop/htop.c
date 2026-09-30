#include "htop.h"
#include "os64/signal.h"

static volatile int stop_signal;
static volatile bool resized;
static void on_stop(int signo) { stop_signal = signo; }
static void on_resize(int signo) { (void)signo; resized = true; }

static uint64_t now_ms(void)
{
    int64_t us = os64_micros();
    if (us >= 0) return (uint64_t)us / 1000;
    os64_ticks_t t = {0}; os64_ticks(&t);
    return t.per_second ? t.ticks / t.per_second * 1000 + t.ticks % t.per_second * 1000 / t.per_second : 0;
}
static int output(const char *p, size_t n)
{
    while (n) {
        int64_t wrote = os64_write(OS64_STDOUT, p, n);
        if (wrote == OS64_INTERRUPTED) { if (stop_signal) return -1; continue; }
        if (wrote <= 0 || (uint64_t)wrote > n) return -1;
        p += wrote; n -= (size_t)wrote;
    }
    return 0;
}
static int literal(const char *s) { return output(s, os64_strlen(s)); }

// Emit changed rows as bounded runs of equal style. Each row includes its
// trailing blanks, so shrinking text erases itself without a whole-screen flash.
static int paint(ht_view_t *v, ht_cell_t *next, ht_cell_t *previous, bool clear)
{
    ht_draw(v, next);
    if (clear) {
        if (literal("\033[0;37;40m\033[2J\033[H") < 0) return -1;
        os64_memset(previous, 255, sizeof(*previous) * HT_WIDTH * HT_HEIGHT);
    }
    for (unsigned y = 0; y < v->lines; y++) {
        if (resized || stop_signal) return 0;
        ht_cell_t *row = next + y * HT_WIDTH, *old = previous + y * HT_WIDTH;
        if (!os64_memcmp(row, old, v->cols * sizeof(*row))) continue;
        char buffer[HT_WIDTH * 24 + 64];
        size_t used = (size_t)os64_snprintf(buffer, sizeof(buffer), "\033[%u;1H", y + 1);
        unsigned x = 0;
        while (x < v->cols) {
            unsigned style = row[x].style; const char *sgr = ht_style(style);
            size_t n = os64_strlen(sgr); os64_memcpy(buffer + used, sgr, n); used += n;
            do { buffer[used++] = (char)row[x++].ch; } while (x < v->cols && row[x].style == style);
        }
        if (output(buffer, used) < 0) return -1;
        os64_memcpy(old, row, v->cols * sizeof(*row));
    }
    char cursor[48];
    os64_snprintf(cursor, sizeof(cursor), "\033[0;37;40m\033[%u;%uH", v->lines ? v->lines : 1, v->cols ? v->cols : 1);
    if (literal(cursor) < 0) return -1;
    if (v->mode == HT_CONFIRM && v->cols >= 59 && v->lines >= 18) v->confirm_drawn = true;
    return 0;
}
static bool discard_pending(int32_t keys)
{
    // Keystrokes queued before a confirmation was visible cannot approve it.
    // Bound the drain; an input flood cancels the question instead of delaying
    // the clock indefinitely or leaving a queued 'y' to approve a later frame.
    char bytes[64];
    for (unsigned i = 0; i < 64; i++) {
        int64_t n = os64_read_for(keys, bytes, sizeof(bytes), 0);
        if (n == OS64_ERR_TIMEOUT) return true;
        if (n == OS64_INTERRUPTED) continue;
        if (n <= 0) return false;
    }
    return false;
}
int main(int argc, char **argv)
{
    bool tree = false, threads = false, idle = false, zombies = false;
    const char *delay = NULL, *positional = NULL;
    const os64_optspec_t specs[] = {
        {'d', "delay", true, "Refresh milliseconds (100..60000; default 1000)", .value_out = &delay},
        {'t', "threads", false, "Expand multithreaded tasks", .flag = &threads},
        {'v', "tree", false, "Show parent/child relationships", .flag = &tree},
        {'i', "idle", false, "Include the CPU idle tasks", .flag = &idle},
        {'z', "zombies", false, "Include zombie tasks", .flag = &zombies}
    };
    os64_args_t args; os64_args_init(&args, argc, argv, specs, sizeof(specs) / sizeof(specs[0]));
    args.about = "An interactive view of os64's tasks, CPU time and memory";
    args.details = "Arrows select; / filters; Enter shows details; ? lists keys; q quits.";
    int32_t parsed = os64_args_parse(&args, "htop", &positional, 1);
    if (parsed == OS64_ARG_HELP) return 0;
    if (parsed != 0) { if (parsed > 0) os64_args_help(&args, "htop"); return 2; }
    uint64_t delay_ms = 1000;
    if (delay && (!os64_parse_u64(delay, &delay_ms) || delay_ms < 100 || delay_ms > 60000)) {
        os64_hprintf(OS64_STDERR, "htop: delay must be 100..60000 milliseconds\n"); return 2;
    }
    os64_tty_info_t tty;
    if (os64_tty_read(&tty) < 0 || !tty.cols || !tty.rows) {
        os64_hprintf(OS64_STDERR, "htop: a terminal is required\n"); return 1;
    }
    int32_t keys = (int32_t)os64_tty_handle();
    os64_monitor_t *monitor = os64_monitor_create(HT_TASKS, HT_THREADS, HT_CORES);
    ht_view_t *v = os64_malloc(sizeof(*v));
    ht_cell_t *grid = os64_malloc(sizeof(*grid) * HT_WIDTH * HT_HEIGHT);
    ht_cell_t *previous = os64_malloc(sizeof(*previous) * HT_WIDTH * HT_HEIGHT);
    char *command = NULL;
    int status = 0; bool raw = false, was_raw = false, entered = false;
    if (keys < 0 || !monitor || !v || !grid || !previous) {
        os64_hprintf(OS64_STDERR, "htop: cannot allocate monitor or terminal handle\n"); status = 1; goto done;
    }
    ht_init(v, os64_taskid()); v->delay_ms = (unsigned)delay_ms;
    v->tree = tree; v->threads = threads; v->idle = idle; v->zombies = zombies;
    ht_layout(v, tty.cols, tty.rows);
    if (os64_signal_set_handler(OS64_SIGINT, on_stop) < 0 ||
        os64_signal_set_handler(OS64_SIGTERM, on_stop) < 0 ||
        os64_signal_set_handler(OS64_SIGHUP, on_stop) < 0 ||
        os64_signal_set_handler(OS64_SIGWINCH, on_resize) < 0) {
        os64_hprintf(OS64_STDERR, "htop: cannot install terminal handlers\n"); status = 1; goto done;
    }
    if (os64_tty_mode(&was_raw, NULL) < 0 || os64_tty_set_raw(true) < 0) {
        os64_hprintf(OS64_STDERR, "htop: run in the terminal foreground\n"); status = 1; goto done;
    }
    raw = true; entered = true;
    ht_input_t input = {.pending = -1};
    uint64_t next_sample = 0, next_geometry = 0, escape_at = 0;
    bool dirty = true, clear = true;
    while (!v->quit && !stop_signal) {
        uint64_t now = now_ms();
        if (resized || now >= next_geometry) {
            bool notified = resized;
            resized = false;
            if (os64_tty_read(&tty) == 0) {
                unsigned cols = v->cols, lines = v->lines;
                ht_layout(v, tty.cols, tty.rows);
                if (notified || cols != v->cols || lines != v->lines) { dirty = clear = true; v->confirm_drawn = false; }
            }
            next_geometry = now + 250;
        }
        if (!v->paused && now >= next_sample) {
            const os64_monitor_snapshot_t *s = NULL;
            int32_t result = os64_monitor_sample(monitor, v->threads, &s);
            ht_rebuild(v, s, result == 0);
            if (result < 0) os64_strcopy(v->notice, sizeof(v->notice), "Sample failed; retrying. No stale measurements shown.");
            next_sample = now_ms() + v->delay_ms; dirty = true;
        }
        if (v->mode != HT_DETAILS && command) { os64_free(command); command = NULL; v->command = NULL; }
        if (dirty) {
            bool confirmation_pending = v->mode == HT_CONFIRM && !v->confirm_drawn;
            if (paint(v, grid, previous, clear) < 0) { status = 1; break; }
            dirty = clear = false;
            // Also discard bytes that arrived during the first paint. They
            // cannot be evidence of a response to the completed question.
            if (confirmation_pending && v->confirm_drawn && !discard_pending(keys)) {
                v->mode = HT_LIST;
                os64_strcopy(v->notice, sizeof(v->notice), "Confirmation cancelled: input was still queued.");
                dirty = true;
                continue;
            }
        }
        unsigned char byte = 0; int key = 0;
        if (input.pending >= 0) { byte = (unsigned char)input.pending; input.pending = -1; key = ht_decode(&input, byte); }
        else {
            int64_t n = os64_read_for(keys, &byte, 1, 40);
            if (n == OS64_ERR_TIMEOUT) {
                if (input.state && now_ms() - escape_at >= 40) key = ht_escape_timeout(&input);
            } else if (n == OS64_INTERRUPTED) continue;
            else if (n != 1) break;
            else { key = ht_decode(&input, byte); if (input.state) escape_at = now_ms(); }
        }
        if (!key) continue;
        if (key == 3) status = OS64_EXIT_FOR_SIGNAL(OS64_SIGINT);
        ht_mode_t old_mode = v->mode; bool was_paused = v->paused; unsigned old_delay = v->delay_ms;
        int action = ht_key(v, key); dirty = true;
        if (v->mode == HT_CONFIRM && old_mode != HT_CONFIRM) {
            input = (ht_input_t){.pending = -1};
            if (!discard_pending(keys)) { v->mode = HT_LIST; os64_strcopy(v->notice, sizeof(v->notice), "Confirmation cancelled: input was still queued."); }
        }
        if ((!v->paused && was_paused) || v->delay_ms != old_delay || (action == HT_REFRESH_THREADS && !v->paused)) next_sample = 0;
        if (action == HT_REFRESH_THREADS && v->paused) os64_strcopy(v->notice, sizeof(v->notice), "Thread sampling changes take effect on resume.");
        if (action == HT_LOAD_DETAILS) {
            os64_free(command); command = NULL;
            const os64_monitor_task_t *t = ht_selected_task(v);
            if (t) os64_proc_command(t->info.pid, &command);
            v->command = command;
        }
        if (action == HT_SEND_SIGNAL) {
            int result = ht_send_signal(v);
            if (result == 0)
                os64_snprintf(v->notice, sizeof(v->notice), "%s sent to task %lu.", v->action_kill ? "Kill" : "Interrupt", v->action_pid);
            else
                os64_snprintf(v->notice, sizeof(v->notice), "Task %lu ended, changed, or refused the signal.", v->action_pid);
            if (!v->paused) next_sample = 0;
        }
    }
    if (stop_signal) status = OS64_EXIT_FOR_SIGNAL(stop_signal);
done:
    if (entered) {
        // Return the terminal's default paper and pen before erasing our frame.
        stop_signal = 0; literal("\033[0m\033[2J\033[H");
    }
    if (raw) os64_tty_set_raw(was_raw);
    if (keys >= 0) os64_close(keys);
    os64_free(command); os64_free(previous); os64_free(grid); os64_free(v);
    os64_monitor_destroy(monitor);
    return status;
}
