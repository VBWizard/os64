// kill — raise one of os64's task signals through /proc/<pid>/ctl.

#include "os64/os64.h"

#define KILL_MAX_PIDS 512
#define KILL_PATH_MAX 64

typedef struct {
    int number;
    const char *name;
    const char *verb;
} kill_signal_t;

static const kill_signal_t kSignals[] = {
    {2, "SIGINT",  "interrupt"},
    {9, "SIGKILL", "kill"},
};

static bool text_equal_folded(const char *left, const char *right)
{
    while (*left != '\0' && *right != '\0')
    {
        char a = *left++;
        char b = *right++;
        if (a >= 'a' && a <= 'z')
            a = (char)(a - 'a' + 'A');
        if (b >= 'a' && b <= 'z')
            b = (char)(b - 'a' + 'A');
        if (a != b)
            return false;
    }
    return *left == '\0' && *right == '\0';
}

static const kill_signal_t *find_signal(const char *text)
{
    if (text == NULL || *text == '\0')
        return NULL;

    for (size_t i = 0; i < sizeof(kSignals) / sizeof(kSignals[0]); i++)
    {
        if ((text[0] == (char)('0' + kSignals[i].number) && text[1] == '\0') ||
            text_equal_folded(text, kSignals[i].name) ||
            text_equal_folded(text, kSignals[i].name + 3))
            return &kSignals[i];
    }
    return NULL;
}

static int send_signal(const char *pid_text, const kill_signal_t *signal)
{
    uint64_t pid;
    if (!os64_parse_u64(pid_text, &pid) || pid == 0)
    {
        os64_hprintf(OS64_STDERR, "kill: invalid PID: %s\n", pid_text);
        return 1;
    }

    char path[KILL_PATH_MAX];
    int32_t wanted = os64_snprintf(path, sizeof(path), "/proc/%lu/ctl", pid);
    if (wanted < 0 || (size_t)wanted >= sizeof(path))
    {
        os64_hprintf(OS64_STDERR, "kill: PID is too large: %s\n", pid_text);
        return 1;
    }

    int64_t handle = os64_open(path, "w");
    if (handle < 0)
    {
        os64_hprintf(OS64_STDERR, "kill: cannot signal PID %lu\n", pid);
        return 1;
    }

    size_t length = os64_strlen(signal->verb);
    int64_t written = os64_write((int32_t)handle, signal->verb, length);
    int64_t closed = os64_close((int32_t)handle);
    if (written != (int64_t)length || closed < 0)
    {
        os64_hprintf(OS64_STDERR, "kill: cannot send %s to PID %lu\n",
                     signal->name, pid);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    bool list = false;
    const char *signal_text = NULL;
    const char *pids[KILL_MAX_PIDS];
    const os64_optspec_t specs[] = {
        {'l', "list", false, "list supported signals", .flag = &list},
        {'s', "signal", true, "signal number or name (default SIGKILL)",
         .value_out = &signal_text, .numeric_alias = true},
    };
    os64_args_t args = {0};
    os64_args_init(&args, argc, argv, specs, 2);
    args.about = "Send a signal to one or more tasks.";
    args.details = "Signals may be written as 2, 9, INT, KILL, SIGINT, or "
                   "SIGKILL. The default is SIGKILL.";

    int32_t parsed = os64_args_parse(
        &args, "kill [-l] [-s SIGNAL | -2 | -9] PID...",
        pids, KILL_MAX_PIDS);
    if (parsed == OS64_ARG_HELP)
        return 0;
    if (parsed < 0)
        return 2;

    if (list)
    {
        if (signal_text != NULL || parsed != 0)
        {
            os64_hprintf(OS64_STDERR,
                         "kill: -l does not accept a signal or PID\n");
            return 2;
        }
        os64_puts("2) SIGINT\n9) SIGKILL\n");
        return 0;
    }

    if (parsed == 0)
    {
        os64_hprintf(OS64_STDERR, "kill: missing PID operand\n");
        os64_args_help(&args, "kill [-l] [-s SIGNAL | -2 | -9] PID...");
        return 2;
    }

    const kill_signal_t *signal = signal_text == NULL ? &kSignals[1] :
                                                        find_signal(signal_text);
    if (signal == NULL)
    {
        os64_hprintf(OS64_STDERR, "kill: unsupported signal: %s\n", signal_text);
        return 2;
    }

    int status = 0;
    for (int32_t i = 0; i < parsed; i++)
        if (send_signal(pids[i], signal) != 0)
            status = 1;
    return status;
}
