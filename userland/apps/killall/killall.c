// killall — signal user tasks by their /proc status name.
#include "os64/os64.h"

#define MAX_NAMES 512
#define STATUS_LINE_MAX 512

typedef struct {
    uint64_t pid;
    char name[STATUS_LINE_MAX];
} target_t;

static bool matches(const char *name, const char *pattern, bool substring)
{
    if (!substring)
        return os64_streq(name, pattern);
    size_t length = os64_strlen(pattern);
    for (; *name; name++)
    {
        size_t i = 0;
        while (i < length && name[i] && name[i] == pattern[i])
            i++;
        if (i == length)
            return true;
    }
    return false;
}

// The shared display reader truncates names to 63 bytes. Read the status
// directly so an exact match cannot silently become a prefix match.
static bool read_name(uint64_t pid, char name[STATUS_LINE_MAX])
{
    char path[64], line[STATUS_LINE_MAX];
    os64_snprintf(path, sizeof(path), "/proc/%lu/status", pid);
    int32_t handle = (int32_t)os64_open(path, NULL);
    if (handle < 0)
        return false; // A task may exit between readdir and open.
    bool have_name = false, user = false, live = false, valid = true;
    int64_t result;
    while ((result = os64_readline(handle, line, sizeof(line))) == 1)
    {
        // readline drains but truncates overlong lines; reject ambiguous data.
        if (os64_strlen(line) == sizeof(line) - 1)
        {
            valid = false;
            continue;
        }
        char *value = line;
        while (*value && *value != '\t') value++;
        if (!*value) continue;
        *value++ = '\0';
        if (os64_streq(line, "name"))
        {
            os64_strcopy(name, STATUS_LINE_MAX, value);
            have_name = *name != '\0';
        }
        else if (os64_streq(line, "kernel"))
            user = os64_streq(value, "no");
        else if (os64_streq(line, "state"))
            live = !os64_streq(value, "zombie") && !os64_streq(value, "none");
    }
    int64_t closed = os64_close(handle);
    return valid && result == 0 && closed == 0 && have_name && user && live;
}

static int send_signal(uint64_t pid, const char *name, const char *verb)
{
    char path[64];
    os64_snprintf(path, sizeof(path), "/proc/%lu/ctl", pid);
    int32_t handle = (int32_t)os64_open(path, "w");
    if (handle >= 0)
    {
        size_t length = os64_strlen(verb);
        // ctl writes are commands: retrying a partial write as a second
        // command would not complete the original operation.
        int64_t written = os64_write(handle, verb, length);
        int64_t closed = os64_close(handle);
        if (written == (int64_t)length && closed == 0)
            return 0;
    }
    os64_hprintf(OS64_STDERR, "killall: cannot signal %s (PID %lu)\n", name, pid);
    return 1;
}

int main(int argc, char **argv)
{
    bool substring = false, list = false;
    const char *signal_text = NULL;
    const char *names[MAX_NAMES];
    bool signaled[MAX_NAMES] = {0};
    const os64_optspec_t specs[] = {
        {'\0', "substring", false, "match literal substrings of task names", .flag = &substring},
        {'l', "list", false, "list supported signals", .flag = &list},
        {'s', "signal", true, "signal number or name (default SIGKILL)",
         .value_out = &signal_text, .numeric_alias = true},
    };
    const char *usage = "killall [--substring] [-s SIGNAL | -2 | -9] NAME...";
    os64_args_t args = {0};
    os64_args_init(&args, argc, argv, specs, 3);
    args.about = "Send a signal to user tasks with matching names.";
    args.details = "Names match exactly and case-sensitively by default, without paths "
                   "or arguments. --substring enables literal substring matching. "
                   "Skips itself, kernel tasks, and zombies. Signals: 2/INT/SIGINT "
                   "or 9/KILL/SIGKILL; default SIGKILL, as with kill.";
    int32_t count = os64_args_parse(&args, usage, names, MAX_NAMES);
    if (count == OS64_ARG_HELP) return 0;
    if (count < 0) return 2;
    if (list)
    {
        if (count || substring || signal_text)
        {
            os64_hprintf(OS64_STDERR, "killall: -l does not accept names or other options\n");
            return 2;
        }
        os64_puts("2) SIGINT\n9) SIGKILL\n");
        return 0;
    }
    if (!count)
    {
        os64_hprintf(OS64_STDERR, "killall: missing name operand\n");
        return 2;
    }
    for (int32_t i = 0; i < count; i++)
        if (!*names[i])
        {
            os64_hprintf(OS64_STDERR, "killall: empty name operand\n");
            return 2;
        }
    const char *verb = "kill";
    if (signal_text)
    {
        if (os64_streq(signal_text, "2") || os64_streq_nocase(signal_text, "INT") ||
            os64_streq_nocase(signal_text, "SIGINT"))
            verb = "interrupt";
        else if (!os64_streq(signal_text, "9") && !os64_streq_nocase(signal_text, "KILL") &&
                 !os64_streq_nocase(signal_text, "SIGKILL"))
        {
            os64_hprintf(OS64_STDERR, "killall: unsupported signal: %s\n", signal_text);
            return 2;
        }
    }
    int32_t directory = (int32_t)os64_opendir("/proc");
    if (directory < 0)
    {
        os64_hprintf(OS64_STDERR, "killall: cannot read /proc\n");
        return 1;
    }
    uint64_t self = os64_taskid();
    os64_dirent_t entry;
    int64_t result;
    target_t *targets = NULL;
    size_t target_count = 0, capacity = 0;
    // Finish selection before signaling: /proc is a live listing, and a
    // supervisor may append replacements in response to our signals.
    while ((result = os64_readdir(directory, &entry)) == 1)
    {
        uint64_t pid;
        char name[STATUS_LINE_MAX];
        if (!os64_parse_u64(entry.name, &pid) || !pid || pid == self ||
            !read_name(pid, name))
            continue;
        bool selected = false;
        for (int32_t i = 0; i < count; i++)
            if (matches(name, names[i], substring)) selected = true;
        if (!selected) continue;
        if (target_count == capacity)
        {
            size_t next_capacity = capacity ? capacity * 2 : 64;
            target_t *grown = NULL;
            if (capacity <= SIZE_MAX / sizeof(*targets) / 2)
                grown = os64_realloc(targets, next_capacity * sizeof(*targets));
            if (!grown)
            {
                os64_hprintf(OS64_STDERR, "killall: cannot allocate task list\n");
                os64_close(directory);
                os64_free(targets);
                return 1;
            }
            targets = grown;
            capacity = next_capacity;
        }
        targets[target_count].pid = pid;
        os64_strcopy(targets[target_count].name, STATUS_LINE_MAX, name);
        target_count++;
    }
    int64_t closed = os64_close(directory);
    if (result < 0 || closed < 0)
    {
        os64_hprintf(OS64_STDERR, "killall: error reading /proc\n");
        os64_free(targets);
        return 1;
    }
    int status = 0;
    for (size_t t = 0; t < target_count; t++)
    {
        if (send_signal(targets[t].pid, targets[t].name, verb))
        {
            status = 1;
            continue;
        }
        // Overlapping operands account for the same successful signal;
        // each task receives one command per invocation.
        for (int32_t i = 0; i < count; i++)
            if (matches(targets[t].name, names[i], substring)) signaled[i] = true;
    }
    os64_free(targets);
    for (int32_t i = 0; i < count; i++)
        if (!signaled[i])
        {
            os64_hprintf(OS64_STDERR, "killall: %s: no task signaled\n", names[i]);
            status = 1;
        }
    return status;
}
