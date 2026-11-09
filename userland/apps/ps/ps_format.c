#include "ps.h"

static bool selected(const os64_proc_info_t *task, const ps_options_t *options)
{
    if (options->pid_selected && task->pid != options->pid)
        return false;
    if (!options->all && task->kernel)
        return false;
    if (!options->pid_selected && !options->all_ttys && !options->all &&
        task->tty != options->current_tty)
        return false;
    if (options->zombies && task->state != OS64_PROC_ZOMBIE)
        return false;
    return true;
}

static void format_time(uint64_t usec, char *out, size_t capacity)
{
    uint64_t seconds = usec / 1000000;
    os64_snprintf(out, capacity, "%lu:%02lu.%01lu",
                  seconds / 60, seconds % 60, (usec % 1000000) / 100000);
}

// Command text can exceed printf's formatting buffer and a pipe write.
static void print_command(const char *text)
{
    size_t left = os64_strlen(text);
    while (left)
    {
        int64_t written = os64_write(OS64_STDOUT, text, left);
        if (written <= 0) return;
        text += written;
        left -= (size_t)written;
    }
}

static void print_task(const os64_proc_info_t *task, const ps_options_t *options,
                       uint32_t depth)
{
    char time[24];
    format_time(task->runtime_us, time, sizeof(time));
    char marker = task->foreground ? '+' : task->shell ? 's' : ' ';
    char *full_command = NULL;
    bool command_failed = options->full && os64_proc_command(task->pid, &full_command) < 0;
    const char *command = full_command ? full_command : task->name;

    if (options->full)
        os64_printf("%5lu %5lu %3u %c%c %4u %3u %9s %6lu %6lu ",
                    task->pid, task->ppid, task->tty,
                    os64_proc_state_letter(task->state), marker,
                    task->core, task->threads, time,
                    task->minor_faults, task->major_faults);
    else
        os64_printf("%5lu %5lu %3u %c%c %9s ", task->pid, task->ppid,
                    task->tty, os64_proc_state_letter(task->state), marker, time);

    if (options->forest)
        for (uint32_t i = 0; i < depth; i++) os64_puts("  ");
    print_command(command[0] ? command : "(none)");
    os64_puts(command_failed ? " [command unavailable]\n" : "\n");
    os64_free(full_command);
}

static void print_threads(const os64_proc_info_t *task)
{
    static os64_thread_info_t threads[PS_MAX_THREADS];
    int32_t count = os64_proc_threads(task->pid, threads, PS_MAX_THREADS);
    for (int32_t i = 0; i < count; i++)
    {
        char time[24];
        format_time(threads[i].runtime_us, time, sizeof(time));
        os64_printf("      %5lu     %c  %4u %3s %9s   `- thread\n",
                    threads[i].tid, os64_proc_state_letter(threads[i].state),
                    threads[i].core,
                    threads[i].affinity[0] ? threads[i].affinity : "?", time);
    }
}

static void print_tree(const os64_proc_info_t *tasks, int32_t count,
                       const ps_options_t *options, uint64_t parent,
                       uint32_t depth, bool *printed)
{
    if (depth > (uint32_t)count) return;
    for (int32_t i = 0; i < count; i++)
    {
        if (printed[i] || !selected(&tasks[i], options) || tasks[i].ppid != parent)
            continue;
        printed[i] = true;
        print_task(&tasks[i], options, depth);
        if (options->threads) print_threads(&tasks[i]);
        print_tree(tasks, count, options, tasks[i].pid, depth + 1, printed);
    }
}

void ps_print(const os64_proc_info_t *tasks, int32_t count,
              const ps_options_t *options)
{
    if (options->full)
        os64_puts("  PID  PPID TTY ST CORE THR      TIME MINFLT MAJFLT COMMAND\n");
    else
        os64_puts("  PID  PPID TTY ST      TIME COMMAND\n");

    bool printed[PS_MAX_TASKS] = {0};
    if (options->forest)
    {
        // PID 0 is the natural root. A final pass preserves tasks whose parent
        // vanished or was filtered out instead of silently losing them.
        print_tree(tasks, count, options, 0, 0, printed);
    }
    for (int32_t i = 0; i < count; i++)
    {
        if (printed[i] || !selected(&tasks[i], options)) continue;
        printed[i] = true;
        print_task(&tasks[i], options, 0);
        if (options->threads) print_threads(&tasks[i]);
        if (options->forest)
            print_tree(tasks, count, options, tasks[i].pid, 1, printed);
    }
}
