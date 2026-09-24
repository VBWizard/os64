// Exercise the real command/parser against a synthetic /proc and ctl sink.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define main killall_main
#include "../userland/apps/killall/killall.c"
#undef main

typedef struct {
    const char *name;
    const char *state;
    bool kernel;
    bool vanished;
    unsigned sends;
} task_t;
static task_t tasks[600];
static size_t ntasks, cursor, current, line_number;
static int fail_pid, short_pid, close_pid;
static bool directory_error, open_error, read_error;
static unsigned opens;
static char last_verb[32];

uint64_t os64_taskid(void) { return 1; }
int64_t os64_opendir(const char *path)
{
    assert(strcmp(path, "/proc") == 0);
    opens++;
    cursor = 0;
    return open_error ? -1 : 3;
}
int64_t os64_readdir(int32_t handle, os64_dirent_t *entry)
{
    assert(handle == 3);
    if (cursor == ntasks) return directory_error ? -1 : 0;
    snprintf(entry->name, sizeof(entry->name), "%zu", ++cursor);
    return 1;
}
int64_t os64_open(const char *path, const char *mode)
{
    unsigned long pid;
    char leaf[32];
    assert(sscanf(path, "/proc/%lu/%31s", &pid, leaf) == 2);
    assert(pid > 0 && pid <= ntasks);
    current = pid - 1;
    if (!mode)
    {
        assert(strcmp(leaf, "status") == 0);
        line_number = 0;
        return tasks[current].vanished ? -1 : 4;
    }
    assert(strcmp(leaf, "ctl") == 0 && strcmp(mode, "w") == 0);
    return (int)pid == fail_pid ? -1 : 5;
}
int64_t os64_readline(int32_t handle, char *out, size_t cap)
{
    assert(handle == 4);
    if (read_error) return -1;
    task_t *t = &tasks[current];
    switch (line_number++) {
        case 0: snprintf(out, cap, "name\t%s", t->name); break;
        case 1: snprintf(out, cap, "kernel\t%s", t->kernel ? "yes" : "no"); break;
        case 2: snprintf(out, cap, "state\t%s", t->state); break;
        default: return 0;
    }
    return 1;
}
int64_t os64_close(int32_t handle)
{
    return handle == 5 && (int)current + 1 == close_pid ? -1 : 0;
}
int64_t os64_write(int32_t handle, const void *data, size_t size)
{
    if (handle == 1 || handle == 2) return (int64_t)size;
    assert(handle == 5 && size < sizeof(last_verb));
    tasks[current].sends++;
    memcpy(last_verb, data, size);
    last_verb[size] = 0;
    return (int)current + 1 == short_pid ? (int64_t)size - 1 : (int64_t)size;
}
int64_t os64_puts(const char *s) { return (int64_t)strlen(s); }
static void reset(void)
{
    memset(tasks, 0, sizeof(tasks));
    const char *names[] = {"worker", "worker", "worker", "worker-extra", "Worker", "worker", "worker", "worker"};
    ntasks = sizeof(names) / sizeof(names[0]);
    for (size_t i = 0; i < ntasks; i++)
        tasks[i] = (task_t){.name = names[i], .state = "runnable"};
    tasks[5].kernel = true;
    tasks[6].state = "zombie";
    tasks[7].vanished = true;
    fail_pid = short_pid = close_pid = 0;
    directory_error = open_error = read_error = false;
    opens = 0;
}
#define RUN(expected, ...) do { \
    char *argv[] = {"killall", __VA_ARGS__}; \
    assert(killall_main(sizeof(argv) / sizeof(argv[0]), argv) == (expected)); \
} while (0)
static unsigned sent(void)
{
    unsigned n = 0;
    for (size_t i = 0; i < ntasks; i++) n += tasks[i].sends;
    return n;
}
int main(void)
{
    reset(); RUN(0, "worker"); assert(sent() == 2 && tasks[1].sends && tasks[2].sends);
    assert(strcmp(last_verb, "kill") == 0);
    reset(); RUN(0, "--substring", "work", "worker"); assert(sent() == 3);
    reset(); RUN(0, "-2", "worker", "worker"); assert(sent() == 2 && strcmp(last_verb, "interrupt") == 0);
    reset(); RUN(0, "--signal=sIgInT", "worker"); assert(sent() == 2);
    reset(); RUN(0, "-s", "KILL", "worker"); assert(sent() == 2);
    reset(); RUN(1, "work"); assert(!sent());
    reset(); RUN(1, "worker", "absent"); assert(sent() == 2);
    reset(); RUN(2, "--substring", ""); assert(!opens);
    reset(); RUN(2, "worker", "-s", "TERM"); assert(!opens);
    reset(); RUN(2, "--bogus", "worker"); assert(!opens);
    reset(); RUN(2, "-s"); assert(!opens);
    reset(); RUN(0, "--help"); assert(!opens);
    reset(); RUN(0, "-l"); assert(!opens);
    reset(); RUN(2, "-l", "worker"); assert(!opens);
    reset(); tasks[1].name = "-worker"; RUN(0, "--", "-worker"); assert(sent() == 1);
    reset(); tasks[1].name = "work.er"; RUN(1, "--substring", "work.*"); assert(!sent());
    reset(); open_error = true; RUN(1, "worker"); assert(!sent());
    reset(); directory_error = true; RUN(1, "worker"); assert(sent() == 2);
    reset(); read_error = true; RUN(1, "worker"); assert(!sent());
    reset(); fail_pid = 2; RUN(1, "worker"); assert(tasks[2].sends == 1);
    reset(); short_pid = 2; RUN(1, "worker"); assert(sent() == 2);
    reset(); close_pid = 2; RUN(1, "worker"); assert(sent() == 2);
    char long_name[128], prefix[64], oversized[600];
    memset(long_name, 'a', sizeof(long_name) - 1); long_name[127] = 0;
    memset(prefix, 'a', sizeof(prefix) - 1); prefix[63] = 0;
    reset(); tasks[1].name = long_name; RUN(1, prefix); assert(!sent());
    reset(); tasks[1].name = long_name; RUN(0, long_name); assert(sent() == 1);
    memset(oversized, 'a', sizeof(oversized) - 1); oversized[599] = 0;
    reset(); tasks[1].name = oversized; RUN(1, "--substring", "aaa"); assert(!sent());
    reset(); ntasks = 600;
    for (size_t i = 0; i < ntasks; i++) tasks[i] = (task_t){.name = "worker", .state = "usleep"};
    RUN(0, "worker"); assert(sent() == 599);
    puts("killall host tests passed");
    return 0;
}
