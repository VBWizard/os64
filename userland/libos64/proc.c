// libos64 proc.c — process control veneer (scaffolding: yield only for now).

#include "os64/proc.h"
#include "os64/syscall.h"

void os64_yield(void)
{
    os64_syscall0(SYSCALL_YIELD);
}

void os64_exit(int32_t code)
{
    os64_syscall1(SYSCALL_EXIT, (uint64_t)(unsigned int)code);
    __builtin_unreachable();
}

int64_t os64_getcwd(char *buf, size_t len)
{
    return (long)os64_syscall2(SYSCALL_GETCWD, (uint64_t)buf, (uint64_t)len);
}

int64_t os64_chdir(const char *path)
{
    return (long)os64_syscall1(SYSCALL_CHDIR, (uint64_t)path);
}

int64_t os64_spawn(const char *path, char *const argv[])
{
    // -1/-1/-1: no redirection, all three streams stay on the console.
    // flags 0: an ordinary foreground child.
    return os64_spawn_redirected(path, argv, -1, -1, -1, 0);
}

int64_t os64_spawn_redirected(const char *path, char *const argv[],
                           int32_t in, int32_t out, int32_t err,
                           uint64_t flags)
{
    // All six argument registers are spoken for now: arg5 carries OS64_SPAWN_*
    // (it used to be a zero the kernel ignored). The dispatcher only
    // pointer-checks the args the table's mask marks, which is just path and
    // argv — flags is a value, not a pointer, and must not be range-checked.
    // (Widened to 64 bits with PTY.md: SET_TTY's master handle rides the
    // high half of the same word — see syscall_numbers.h for why.)
    return (long)os64_syscall6(SYSCALL_SPAWN,
                               (uint64_t)path,
                               (uint64_t)argv,
                               (uint64_t)(int64_t)in,
                               (uint64_t)(int64_t)out,
                               (uint64_t)(int64_t)err,
                               flags);
}

int64_t os64_spawn_seated(const char *path, char *const argv[], int64_t master)
{
    // Seat the child on the master's pty slave (PTY.md): it becomes the
    // slave's SHELL — controlling terminal, foreground, the works — and its
    // console handles route there without one line of pty-awareness in the
    // child. No redirections: slots 0/1/2 stay "the console", which IS the
    // slave now. Same trick pipelines pull, one seam over.
    return os64_spawn_redirected(path, argv, -1, -1, -1,
                                 OS64_SPAWN_SET_TTY |
                                 ((uint64_t)(uint32_t)master << OS64_SPAWN_TTY_SHIFT));
}

int64_t os64_wait(int64_t pid, int32_t *exit_code)
{
    return (long)os64_syscall2(SYSCALL_WAIT, (uint64_t)pid, (uint64_t)exit_code);
}

int64_t os64_reap(int32_t *exit_code)
{
    return (long)os64_syscall1(SYSCALL_REAP, (uint64_t)exit_code);
}

uint64_t os64_taskid(void)
{
    return os64_syscall0(SYSCALL_TASKID);
}

int64_t os64_sleep(uint64_t ms)
{
    return (long)os64_syscall1(SYSCALL_SLEEP, ms);
}

int64_t os64_ticks(os64_ticks_t *out)
{
    return (long)os64_syscall1(SYSCALL_TICKS, (uint64_t)out);
}

int64_t os64_setenv(const char *key, const char *value)
{
    // NULL value would mean "unset" at the syscall — os64_unsetenv is the
    // honest spelling for that, so keep this one meaning exactly "set".
    if (value == NULL)
        return -1;
    return (int64_t)os64_syscall2(SYSCALL_SETENV, (uint64_t)key,
                                  (uint64_t)value);
}

int64_t os64_unsetenv(const char *key)
{
    return (int64_t)os64_syscall2(SYSCALL_SETENV, (uint64_t)key, 0);
}
