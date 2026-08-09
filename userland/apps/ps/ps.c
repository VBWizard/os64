// ps — a one-shot view of the task reports exported by /proc.

#include "ps.h"

static bool parse_pid(const char *text, uint64_t *pid)
{
    if (text == NULL || *text == '\0') return false;
    uint64_t value = 0;
    for (const char *p = text; *p != '\0'; p++)
    {
        if (*p < '0' || *p > '9') return false;
        uint64_t digit = (uint64_t)(*p - '0');
        if (value > (UINT64_MAX - digit) / 10) return false;
        value = value * 10 + digit;
    }
    *pid = value;
    return true;
}

int main(int argc, char **argv)
{
    ps_options_t options = {0};
    const char *pid_text = NULL;
    const os64_optspec_t specs[] = {
        {'a', "all-terminals", false, "show user tasks on every terminal", .flag = &options.all_ttys},
        {'e', "everything", false, "show every task, including kernel tasks", .flag = &options.all},
        {'f', "full", false, "show CPU, thread, fault, and full command details", .flag = &options.full},
        {'T', "threads", false, "show each task's threads", .flag = &options.threads},
        {'p', "pid", true, "show only PID", .value_out = &pid_text},
        {'\0', "forest", false, "show parent/child relationships", .flag = &options.forest},
        {'z', "zombies", false, "show only zombie tasks", .flag = &options.zombies}
    };
    os64_args_t args = {0};
    os64_args_init(&args, argc, argv, specs, 7);
    args.about = "Show a snapshot of the system's tasks.";
    args.details = "By default, ps shows user tasks on its current terminal. "
                   "ST's second character is + for foreground or s for shell.";
    int32_t parsed = os64_args_parse(
        &args, "ps [-aefTz] [-p PID] [--forest]", NULL, 0);
    if (parsed == OS64_ARG_HELP) return 0;
    if (parsed < 0) return 2;

    if (pid_text != NULL)
    {
        if (!parse_pid(pid_text, &options.pid))
        {
            os64_hprintf(OS64_STDERR, "ps: invalid PID: %s\n", pid_text);
            return 2;
        }
        options.pid_selected = true;
        options.all = true; // an explicit PID should not disappear by policy
    }
    // Thread rows carry core, affinity, and their own time. The full heading
    // is the compact vocabulary capable of labelling those values honestly.
    if (options.threads)
        options.full = true;

    static os64_proc_info_t tasks[PS_MAX_TASKS];
    int32_t count = os64_proc_snapshot(tasks, PS_MAX_TASKS);
    if (count < 0)
    {
        os64_hprintf(OS64_STDERR, "ps: cannot read /proc\n");
        return 1;
    }
    uint64_t self = os64_getpid();
    bool found_self = false;
    for (int32_t i = 0; i < count; i++)
        if (tasks[i].pid == self)
        {
            options.current_tty = tasks[i].tty;
            found_self = true;
            break;
        }
    if (!found_self && !options.all_ttys && !options.all &&
        !options.pid_selected)
    {
        os64_hprintf(OS64_STDERR, "ps: cannot identify its current terminal\n");
        return 1;
    }
    ps_print(tasks, count, &options);
    return 0;
}
