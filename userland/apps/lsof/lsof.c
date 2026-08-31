// lsof.c — join the task and system reports that expose open objects.
//
// os64 has no users, device nodes for ordinary files, or statfs-shaped file
// descriptors to query. The columns stop where the kernel's public facts do:
// command, task, descriptor/role, tagged object type, and path/object identity.
// /proc/<pid>/handles and cwd supply task-owned rows; /sys/openfiles supplies
// the disk files held outside every task handle table. File modes and network
// endpoints can join when those reports know them; guessing either here would
// turn a diagnostic into misinformation.

#include "os64/os64.h"
#include "os64/slurp.h"

#define LSOF_MAX_TASKS 512
#define LSOF_MAX_PATHS 64
#define LSOF_PATH_MAX 512
#define LSOF_HANDLES_CAP 16384
#define LSOF_OPENFILES_CAP 65536

typedef struct {
    bool terse;
    bool pidSelected;
    uint64_t pid;
    const char *command;
    int32_t pathCount;
    char paths[LSOF_MAX_PATHS][LSOF_PATH_MAX];
} lsof_options_t;

typedef struct {
    uint64_t ident;
    char *path;
} lsof_claim_t;

typedef struct {
    lsof_claim_t *items;
    size_t count;
    size_t capacity;
} lsof_claims_t;

static os64_proc_info_t tasks[LSOF_MAX_TASKS];
static lsof_options_t options;

static bool command_starts_with(const char *command, const char *prefix)
{
    if (prefix == NULL)
        return true;
    while (*prefix != '\0')
    {
        if (*command++ != *prefix++)
            return false;
    }
    return true;
}

// /proc reports canonical absolute paths. Resolve operands lexically to the
// same spelling without requiring the object still to exist: an open-but-
// unlinked file is exactly the sort of object lsof must still be able to name.
static bool canonical_path(const char *path, char *out, size_t capacity)
{
    char cwd[LSOF_PATH_MAX];
    const char *sources[2];
    int32_t sourceCount = 0;
    size_t length = 0;

    if (path == NULL || *path == '\0' || capacity < 2)
        return false;
    if (path[0] != '/')
    {
        if (os64_getcwd(cwd, sizeof(cwd)) < 0)
            return false;
        sources[sourceCount++] = cwd;
    }
    sources[sourceCount++] = path;

    for (int32_t source = 0; source < sourceCount; source++)
    {
        const char *p = sources[source];
        while (*p != '\0')
        {
            while (*p == '/')
                p++;
            if (*p == '\0')
                break;

            const char *start = p;
            while (*p != '\0' && *p != '/')
                p++;
            size_t component = (size_t)(p - start);

            if (component == 1 && start[0] == '.')
                continue;
            if (component == 2 && start[0] == '.' && start[1] == '.')
            {
                while (length > 0 && out[length - 1] != '/')
                    length--;
                if (length > 0)
                    length--;
                continue;
            }
            if (length + component + 2 > capacity)
                return false;
            out[length++] = '/';
            for (size_t i = 0; i < component; i++)
                out[length++] = start[i];
        }
    }

    if (length == 0)
        out[length++] = '/';
    out[length] = '\0';
    return true;
}

static bool task_selected(const os64_proc_info_t *task,
                          const lsof_options_t *options)
{
    if (options->pidSelected && task->pid != options->pid)
        return false;
    return command_starts_with(task->name, options->command);
}

static bool path_selected(const char *detail, const lsof_options_t *options)
{
    if (options->pathCount == 0)
        return true;
    if (detail == NULL)
        return false;
    for (int32_t i = 0; i < options->pathCount; i++)
        if (os64_streq(detail, options->paths[i]))
            return true;
    return false;
}

static bool console_type(const char *type)
{
    return os64_streq(type, "console-in") ||
           os64_streq(type, "console-out") ||
           os64_streq(type, "console-err");
}

static void print_row(const char *command, const char *pid, const char *fd,
                      const char *type, const char *detail,
                      bool *headingPrinted)
{
    if (!*headingPrinted)
    {
        os64_puts("COMMAND            PID  FD TYPE         NAME\n");
        *headingPrinted = true;
    }
    os64_printf("%-16.15s %5s %3s %-12s %s\n",
                command, pid, fd, type,
                detail != NULL && *detail != '\0' ? detail : "-");
}

static void print_task_match(const os64_proc_info_t *task, const char *fd,
                             const char *type, const char *detail,
                             const lsof_options_t *options,
                             bool *headingPrinted, bool *pidPrinted)
{
    if (options->terse)
    {
        if (!*pidPrinted)
        {
            os64_printf("%lu\n", task->pid);
            *pidPrinted = true;
        }
        return;
    }

    char pid[24];
    os64_snprintf(pid, sizeof(pid), "%lu", task->pid);
    print_row(task->name[0] ? task->name : "(none)", pid, fd, type,
              detail, headingPrinted);
}

static bool claim_exists(const lsof_claims_t *claims, uint64_t ident,
                         const char *path)
{
    for (size_t i = 0; i < claims->count; i++)
        if (claims->items[i].ident == ident &&
            os64_streq(claims->items[i].path, path))
            return true;
    return false;
}

static bool claim_add(lsof_claims_t *claims, uint64_t ident, const char *path)
{
    if (claim_exists(claims, ident, path))
        return true;
    if (claims->count == claims->capacity)
    {
        size_t capacity = claims->capacity == 0 ? 32 : claims->capacity * 2;
        lsof_claim_t *grown = os64_realloc(claims->items,
                                           capacity * sizeof(*grown));
        if (grown == NULL)
            return false;
        claims->items = grown;
        claims->capacity = capacity;
    }

    size_t length = os64_strlen(path) + 1;
    char *copy = os64_malloc(length);
    if (copy == NULL)
        return false;
    os64_memcpy(copy, path, length);
    claims->items[claims->count].ident = ident;
    claims->items[claims->count].path = copy;
    claims->count++;
    return true;
}

static void claims_free(lsof_claims_t *claims)
{
    for (size_t i = 0; i < claims->count; i++)
        os64_free(claims->items[i].path);
    os64_free(claims->items);
}

// Parse "handle<TAB>type[<TAB>detail[<TAB>ident]]" in place. A FILE's
// identity is its final field; taking it from the right leaves tabs inside a
// pathname intact. Unknown future type words remain printable because the
// kernel's text file, not this utility, owns that vocabulary.
static bool parse_handle(char *line, uint64_t *number,
                         char **type, char **detail,
                         uint64_t *ident, bool *hasIdent)
{
    char *firstTab = line;
    while (*firstTab != '\0' && *firstTab != '\t')
        firstTab++;
    if (*firstTab == '\0')
        return false;
    *firstTab++ = '\0';

    char *secondTab = firstTab;
    while (*secondTab != '\0' && *secondTab != '\t' && *secondTab != '\r')
        secondTab++;
    if (*secondTab == '\t')
    {
        *secondTab++ = '\0';
        *detail = secondTab;
        char *end = secondTab;
        while (*end != '\0' && *end != '\r')
            end++;
        *end = '\0';
    }
    else
    {
        *secondTab = '\0';
        *detail = NULL;
    }

    *type = firstTab;
    *hasIdent = false;
    if (os64_streq(*type, "file"))
    {
        if (*detail == NULL)
            return false;
        char *lastTab = NULL;
        for (char *p = *detail; *p != '\0'; p++)
            if (*p == '\t')
                lastTab = p;
        if (lastTab == NULL || !os64_parse_u64(lastTab + 1, ident))
            return false;
        *lastTab = '\0';
        *hasIdent = true;
    }
    return **type != '\0' && os64_parse_u64(line, number);
}

static int list_handles(const os64_proc_info_t *task,
                        const lsof_options_t *options,
                        bool emit, lsof_claims_t *claims,
                        bool *headingPrinted, bool *pidPrinted)
{
    char path[96];
    os64_snprintf(path, sizeof(path), "/proc/%lu/handles", task->pid);

    uint8_t *contents = NULL;
    size_t length = 0;
    os64_slurp_status_t status = os64_slurp(path, LSOF_HANDLES_CAP,
                                            &contents, &length);
    if (status != OS64_SLURP_OK)
    {
        // A task may vanish after the /proc snapshot; that race is ordinary
        // during a system-wide walk. Every other failure means an extant
        // report could not be read and must not disappear from the results.
        if (status == OS64_SLURP_NO_FILE &&
            (!options->pidSelected || task->pid != options->pid))
            return 0;
        os64_hprintf(OS64_STDERR, "lsof: %s: %s\n", path,
                     os64_slurp_status_name(status));
        return -1;
    }

    int matches = 0;
    char *cursor = (char *)contents;
    while ((size_t)(cursor - (char *)contents) < length)
    {
        char *line = cursor;
        while ((size_t)(cursor - (char *)contents) < length && *cursor != '\n')
            cursor++;
        if ((size_t)(cursor - (char *)contents) < length)
            *cursor++ = '\0';
        if (*line == '\0')
            continue;

        uint64_t handle;
        uint64_t ident = 0;
        bool hasIdent;
        char *type;
        char *detail;
        if (!parse_handle(line, &handle, &type, &detail, &ident, &hasIdent))
        {
            os64_hprintf(OS64_STDERR,
                         "lsof: malformed handle row for task %lu\n", task->pid);
            os64_free(contents);
            return -1;
        }
        if (hasIdent && !claim_add(claims, ident, detail))
        {
            os64_hprintf(OS64_STDERR,
                         "lsof: cannot remember open files for task %lu\n",
                         task->pid);
            os64_free(contents);
            return -1;
        }
        if (!emit)
            continue;
        // The three standard console slots are routing machinery, not files
        // or independently identifiable objects. They add three identical
        // routine rows to nearly every task without answering lsof's question.
        if (console_type(type))
            continue;
        if (!path_selected(detail, options))
            continue;

        matches++;
        char fd[24];
        os64_snprintf(fd, sizeof(fd), "%lu", handle);
        print_task_match(task, fd, type, detail, options,
                         headingPrinted, pidPrinted);
    }

    os64_free(contents);
    return matches;
}

static int list_cwd(const os64_proc_info_t *task,
                    const lsof_options_t *options,
                    bool *headingPrinted, bool *pidPrinted)
{
    char report[96];
    os64_snprintf(report, sizeof(report), "/proc/%lu/cwd", task->pid);

    uint8_t *contents = NULL;
    size_t length = 0;
    os64_slurp_status_t status = os64_slurp(report, LSOF_PATH_MAX - 1,
                                            &contents, &length);
    if (status != OS64_SLURP_OK)
    {
        if (status == OS64_SLURP_NO_FILE && !options->pidSelected)
            return 0;
        os64_hprintf(OS64_STDERR, "lsof: %s: %s\n", report,
                     os64_slurp_status_name(status));
        return -1;
    }

    while (length > 0 &&
           (contents[length - 1] == '\n' || contents[length - 1] == '\r'))
        contents[--length] = '\0';
    if (length == 0)
    {
        os64_hprintf(OS64_STDERR,
                     "lsof: empty cwd report for task %lu\n", task->pid);
        os64_free(contents);
        return -1;
    }

    int matches = 0;
    if (path_selected((char *)contents, options))
    {
        matches = 1;
        print_task_match(task, "cwd", "dir", (char *)contents, options,
                         headingPrinted, pidPrinted);
    }
    os64_free(contents);
    return matches;
}

// Split the final whitespace-delimited field from a row. Working from the
// right preserves spaces in the pathname at the front of /sys/openfiles.
static bool take_last_field(char *line, char **field)
{
    size_t length = os64_strlen(line);
    while (length > 0 && (line[length - 1] == ' ' ||
                          line[length - 1] == '\t' ||
                          line[length - 1] == '\r'))
        line[--length] = '\0';
    if (length == 0)
        return false;

    size_t start = length;
    while (start > 0 && line[start - 1] != ' ' && line[start - 1] != '\t')
        start--;
    if (start == 0)
        return false;
    *field = line + start;
    while (start > 0 && (line[start - 1] == ' ' || line[start - 1] == '\t'))
        start--;
    line[start] = '\0';
    return **field != '\0';
}

// Parse "path mount ident handles". Mount validates the public row; handles
// also closes the race where a task acquires a file after the /proc walk.
static bool parse_openfile(char *line, char **path, uint64_t *ident,
                           uint64_t *handles)
{
    char *handlesText;
    char *identText;
    char *mount;
    if (!take_last_field(line, &handlesText) ||
        !take_last_field(line, &identText) ||
        !take_last_field(line, &mount) ||
        !os64_parse_u64(handlesText, handles) ||
        !os64_parse_u64(identText, ident) ||
        *line == '\0' || *mount == '\0')
        return false;
    *path = line;
    return true;
}

static bool kernel_selected(const lsof_options_t *options)
{
    if (options->pidSelected || options->terse)
        return false;
    return command_starts_with("kernel", options->command);
}

static int list_kernel_openfiles(const lsof_options_t *options,
                                 const lsof_claims_t *claims,
                                 bool *headingPrinted)
{
    if (!kernel_selected(options))
        return 0;

    uint8_t *contents = NULL;
    size_t length = 0;
    os64_slurp_status_t status = os64_slurp("/sys/openfiles",
                                            LSOF_OPENFILES_CAP,
                                            &contents, &length);
    if (status != OS64_SLURP_OK)
    {
        os64_hprintf(OS64_STDERR, "lsof: /sys/openfiles: %s\n",
                     os64_slurp_status_name(status));
        return -1;
    }

    int matches = 0;
    char *cursor = (char *)contents;
    while ((size_t)(cursor - (char *)contents) < length)
    {
        char *line = cursor;
        while ((size_t)(cursor - (char *)contents) < length && *cursor != '\n')
            cursor++;
        if ((size_t)(cursor - (char *)contents) < length)
            *cursor++ = '\0';
        if (*line == '\0')
            continue;
        if (*line == '#')
        {
            const char *p = line + 1;
            while (*p == ' ' || *p == '\t')
                p++;
            if (*p >= '0' && *p <= '9')
            {
                os64_hprintf(OS64_STDERR,
                             "lsof: /sys/openfiles listing was truncated\n");
                os64_free(contents);
                return -1;
            }
            continue;
        }

        char *path;
        uint64_t ident;
        uint64_t handles;
        if (!parse_openfile(line, &path, &ident, &handles))
        {
            os64_hprintf(OS64_STDERR,
                         "lsof: malformed /sys/openfiles row\n");
            os64_free(contents);
            return -1;
        }
        if (handles != 0 || claim_exists(claims, ident, path) ||
            !path_selected(path, options))
            continue;

        matches++;
        print_row("kernel", "-", "-", "file", path, headingPrinted);
    }

    os64_free(contents);
    return matches;
}

int main(int argc, char **argv)
{
    const char *pidText = NULL;
    const char *pathOperands[LSOF_MAX_PATHS] = {0};
    const os64_optspec_t specs[] = {
        {'p', "pid", true, "show only task PID", .value_out = &pidText},
        {'c', "command", true, "show commands beginning with COMMAND",
         .value_out = &options.command},
        {'t', "terse", false, "print only matching task IDs",
         .flag = &options.terse}
    };

    os64_args_t args = {0};
    os64_args_init(&args, argc, argv, specs, 3);
    args.about = "List task-owned and kernel-held open files.";
    args.details = "Joins /proc task reports with /sys/openfiles. PATH operands match exact canonical paths.";
    int32_t parsed = os64_args_parse(
        &args, "lsof [-t] [-p PID] [-c COMMAND] [PATH ...]",
        pathOperands, LSOF_MAX_PATHS);
    if (parsed == OS64_ARG_HELP)
        return 0;
    if (parsed < 0)
        return 2;
    options.pathCount = parsed;

    if (pidText != NULL)
    {
        if (!os64_parse_u64(pidText, &options.pid))
        {
            os64_hprintf(OS64_STDERR, "lsof: invalid PID: %s\n", pidText);
            return 2;
        }
        options.pidSelected = true;
    }
    if (options.command != NULL && *options.command == '\0')
    {
        os64_hprintf(OS64_STDERR, "lsof: command prefix cannot be empty\n");
        return 2;
    }
    for (int32_t i = 0; i < options.pathCount; i++)
    {
        if (!canonical_path(pathOperands[i], options.paths[i],
                            sizeof(options.paths[i])))
        {
            os64_hprintf(OS64_STDERR, "lsof: path is too long: %s\n",
                         pathOperands[i]);
            return 2;
        }
    }

    int32_t taskCount = os64_proc_snapshot(tasks, LSOF_MAX_TASKS);
    if (taskCount < 0)
    {
        os64_hprintf(OS64_STDERR, "lsof: cannot read /proc\n");
        return 1;
    }

    bool selectedPidFound = false;
    bool headingPrinted = false;
    int matches = 0;
    bool failed = false;
    lsof_claims_t claims = {0};
    for (int32_t i = 0; i < taskCount; i++)
    {
        if (options.pidSelected && tasks[i].pid == options.pid)
            selectedPidFound = true;
        bool emit = task_selected(&tasks[i], &options);
        bool pidPrinted = false;
        int cwd = emit ? list_cwd(&tasks[i], &options,
                                  &headingPrinted, &pidPrinted) : 0;
        int handled = list_handles(&tasks[i], &options,
                                   emit, &claims,
                                   &headingPrinted, &pidPrinted);
        if (cwd < 0 || handled < 0)
            failed = true;
        else
            matches += cwd + handled;
    }

    if (!failed)
    {
        int kernelHeld = list_kernel_openfiles(&options, &claims,
                                               &headingPrinted);
        if (kernelHeld < 0)
            failed = true;
        else
            matches += kernelHeld;
    }
    claims_free(&claims);

    if (options.pidSelected && !selectedPidFound)
        os64_hprintf(OS64_STDERR, "lsof: no such task: %lu\n", options.pid);
    if (failed || (options.pidSelected && !selectedPidFound))
        return 1;
    return matches > 0 ? 0 : 1;
}
