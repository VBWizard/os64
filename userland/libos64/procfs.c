#include "os64/os64.h"
#include "os64/procfs.h"

#define PROC_LINE_MAX 256
#define PROC_PATH_MAX 96

static void read_command(uint64_t pid, os64_proc_info_t *info);

static bool numeric_name(const char *name)
{
    if (name == NULL || *name == '\0')
        return false;
    for (; *name != '\0'; name++)
        if (*name < '0' || *name > '9')
            return false;
    return true;
}

static char *split_value(char *line)
{
    char *value = line;
    while (*value != '\0' && *value != ' ' && *value != '\t')
        value++;
    if (*value == '\0')
        return NULL;
    *value++ = '\0';
    while (*value == ' ' || *value == '\t')
        value++;
    return value;
}

static os64_proc_state_t parse_state(const char *value)
{
    if (os64_streq(value, "running"))  return OS64_PROC_RUNNING;
    if (os64_streq(value, "runnable")) return OS64_PROC_RUNNABLE;
    if (os64_streq(value, "stopped"))  return OS64_PROC_STOPPED;
    if (os64_streq(value, "usleep"))   return OS64_PROC_USLEEP;
    if (os64_streq(value, "isleep"))   return OS64_PROC_ISLEEP;
    if (os64_streq(value, "zombie"))   return OS64_PROC_ZOMBIE;
    return OS64_PROC_NONE;
}

const char *os64_proc_state_name(os64_proc_state_t state)
{
    static const char *names[] = {
        "none", "running", "runnable", "stopped",
        "usleep", "isleep", "zombie"
    };
    return state <= OS64_PROC_ZOMBIE ? names[state] : "none";
}

char os64_proc_state_letter(os64_proc_state_t state)
{
    // Keep the two sleep reasons distinguishable: U = timed/user sleep,
    // I = interruptible I/O sleep. R is shared by running and runnable.
    static const char letters[] = "?RRSUIZ";
    return state <= OS64_PROC_ZOMBIE ? letters[state] : '?';
}

static int read_task_status(uint64_t pid, os64_proc_info_t *info)
{
    char path[PROC_PATH_MAX];
    char line[PROC_LINE_MAX];
    os64_snprintf(path, sizeof(path), "/proc/%lu/status", pid);
    int32_t h = (int32_t)os64_open(path, NULL);
    if (h < 0)
        return -1;

    info->pid = pid;
    int64_t result;
    while ((result = os64_readline(h, line, sizeof(line))) == 1)
    {
        char *value = split_value(line);
        if (value == NULL)
            continue;
        if (os64_streq(line, "task")) info->pid = os64_atou(value);
        else if (os64_streq(line, "parent")) info->ppid = os64_atou(value);
        else if (os64_streq(line, "name")) os64_strcopy(info->name, sizeof(info->name), value);
        else if (os64_streq(line, "state")) info->state = parse_state(value);
        else if (os64_streq(line, "kernel")) info->kernel = os64_streq(value, "yes");
        else if (os64_streq(line, "tty")) info->tty = (uint32_t)os64_atou(value);
        else if (os64_streq(line, "foreground")) info->foreground = os64_streq(value, "yes");
        else if (os64_streq(line, "shell")) info->shell = os64_streq(value, "yes");
        else if (os64_streq(line, "threads")) info->threads = (uint32_t)os64_atou(value);
        else if (os64_streq(line, "core")) info->core = (uint32_t)os64_atou(value);
        else if (os64_streq(line, "runtime_us")) info->runtime_us = os64_atou(value);
        else if (os64_streq(line, "switches")) info->switches = os64_atou(value);
        else if (os64_streq(line, "faults"))
        {
            info->minor_faults = os64_atou(value);
            while (*value >= '0' && *value <= '9') value++;
            while (*value == ' ' || *value == '\t') value++;
            while (*value != '\0' && (*value < '0' || *value > '9')) value++;
            info->major_faults = os64_atou(value);
        }
    }
    os64_close(h);
    return result < 0 ? -1 : 0;
}

static uint64_t get_heap_value(uint64_t pid)
{
    // Need to retrieve the mapped and virgin values, then
    // subtract virgin from mapped, and return that value

    char path[PROC_PATH_MAX];
    char line[PROC_LINE_MAX];
    int64_t result;
    uint64_t mappedValue = 0, virginValue = 0;

    os64_snprintf(path, sizeof(path), "/proc/%lu/heap", pid);
    int32_t h = (int32_t)os64_open(path, NULL);
    if (h < 0)
        return 0;   // task died mid-scan, or no such file: "no heap to report"
                    // — never a sentinel, this function's return type is a size

    while ((result = os64_readline(h, line, sizeof(line))) == 1)
    {
        char *value = split_value(line);
        if (value == NULL)
            continue;
        if (os64_streq(line, "mapped"))
            mappedValue = os64_atou(value);
        else if (os64_streq(line, "virgin"))
            virginValue = os64_atou(value);
        if (mappedValue > 0 && virginValue > 0)
            break;
    }

    // Close BEFORE returning: this runs once per task per refresh, and with
    // TASK_MAX_HANDLES at 16 a leaked handle here starves top's entire scan
    // (read_task_status's open fails → every row vanishes) within one screen.
    os64_close(h);

    return mappedValue - virginValue;
}

int32_t os64_proc_read(uint64_t pid, os64_proc_info_t *out)
{
    os64_memset(out, 0, sizeof(*out));
    if (read_task_status(pid, out) < 0)
        return -1;
    read_command(pid, out);
    out->heap = get_heap_value(pid);
    return 0;
}

static void read_command(uint64_t pid, os64_proc_info_t *info)
{
    char path[PROC_PATH_MAX];
    char line[PROC_LINE_MAX];
    size_t used = 0;
    os64_snprintf(path, sizeof(path), "/proc/%lu/cmdline", pid);
    int32_t h = (int32_t)os64_open(path, NULL);
    if (h < 0)
        return;

    while (os64_readline(h, line, sizeof(line)) == 1)
    {
        int32_t wanted = os64_snprintf(info->command + used,
            sizeof(info->command) - used, "%s%s", used ? " " : "", line);
        if (wanted < 0 || (size_t)wanted >= sizeof(info->command) - used)
        {
            used = sizeof(info->command) - 1;
            break;
        }
        used += (size_t)wanted;
    }
    os64_close(h);
    if (used == 0)
        os64_strcopy(info->command, sizeof(info->command), info->name);
}

int32_t os64_proc_snapshot(os64_proc_info_t *out, size_t capacity)
{
    int32_t directory = (int32_t)os64_opendir("/proc");
    if (directory < 0)
        return -1;

    size_t count = 0;
    os64_dirent_t entry;
    while (count < capacity && os64_readdir(directory, &entry) == 1)
    {
        if (!numeric_name(entry.name))
            continue;
        uint64_t pid = os64_atou(entry.name);
        if (os64_proc_read(pid, &out[count]) < 0)
            continue;
        count++;
    }
    os64_close(directory);
    return (int32_t)count;
}

static int read_thread_status(const char *path, os64_thread_info_t *info)
{
    char line[PROC_LINE_MAX];
    int32_t h = (int32_t)os64_open(path, NULL);
    if (h < 0) return -1;
    int64_t result;
    while ((result = os64_readline(h, line, sizeof(line))) == 1)
    {
        char *value = split_value(line);
        if (value == NULL) continue;
        if (os64_streq(line, "thread")) info->tid = os64_atou(value);
        else if (os64_streq(line, "task")) info->pid = os64_atou(value);
        else if (os64_streq(line, "state")) info->state = parse_state(value);
        else if (os64_streq(line, "core")) info->core = (uint32_t)os64_atou(value);
        else if (os64_streq(line, "runtime_us")) info->runtime_us = os64_atou(value);
        else if (os64_streq(line, "affinity")) os64_strcopy(info->affinity, sizeof(info->affinity), value);
    }
    os64_close(h);
    return result < 0 ? -1 : 0;
}

int32_t os64_proc_read_thread(uint64_t pid, uint64_t tid,
                              os64_thread_info_t *out)
{
    char path[PROC_PATH_MAX];
    os64_snprintf(path, sizeof(path), "/proc/%lu/thread/%lu/status", pid, tid);
    os64_memset(out, 0, sizeof(*out));
    return read_thread_status(path, out);
}

int32_t os64_proc_threads(uint64_t pid, os64_thread_info_t *out, size_t capacity)
{
    char directory_path[PROC_PATH_MAX];
    os64_snprintf(directory_path, sizeof(directory_path), "/proc/%lu/thread", pid);
    int32_t directory = (int32_t)os64_opendir(directory_path);
    if (directory < 0) return -1;
    size_t count = 0;
    os64_dirent_t entry;
    while (count < capacity && os64_readdir(directory, &entry) == 1)
    {
        if (!numeric_name(entry.name)) continue;
        char path[PROC_PATH_MAX];
        os64_snprintf(path, sizeof(path), "/proc/%lu/thread/%s/status", pid, entry.name);
        os64_memset(&out[count], 0, sizeof(out[count]));
        if (read_thread_status(path, &out[count]) == 0) count++;
    }
    os64_close(directory);
    return (int32_t)count;
}

// The controlling-terminal reader — /proc/self/tty through the same
// ignore-unknown-keys parser as everything else here, so the kernel can grow
// the file new lines without breaking a single existing binary. "self" does
// the process-relativity: no PID, no handle, just "my terminal".
int32_t os64_tty_read(os64_tty_info_t *out)
{
    char line[PROC_LINE_MAX];
    os64_memset(out, 0, sizeof(*out));

    int32_t h = (int32_t)os64_open("/proc/self/tty", NULL);
    if (h < 0)
        return -1;

    int64_t result;
    while ((result = os64_readline(h, line, sizeof(line))) == 1)
    {
        char *value = split_value(line);
        if (value == NULL)
            continue;
        if (os64_streq(line, "tty"))             out->tty = (uint32_t)os64_atou(value);
        else if (os64_streq(line, "rows"))       out->rows = (uint32_t)os64_atou(value);
        else if (os64_streq(line, "cols"))       out->cols = (uint32_t)os64_atou(value);
        else if (os64_streq(line, "focused"))    out->focused = os64_streq(value, "yes");
        else if (os64_streq(line, "state"))      out->live = os64_streq(value, "live");
        else if (os64_streq(line, "scrollback")) out->scrollback = (uint32_t)os64_atou(value);
        else if (os64_streq(line, "fg_task"))    out->fg_task = os64_atou(value);
    }
    os64_close(h);
    return result < 0 ? -1 : 0;
}
