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
    return os64_spawn_redirected(path, argv, -1, -1, -1);
}

int64_t os64_spawn_redirected(const char *path, char *const argv[],
                           int32_t in, int32_t out, int32_t err)
{
    // 5 args (no os64_syscall5 exists, so ride syscall6 with a zero tail — the
    // kernel handler ignores arg5, and the dispatcher only pointer-checks the
    // args the table's mask marks, which is just path and argv).
    return (long)os64_syscall6(SYSCALL_SPAWN,
                               (uint64_t)path,
                               (uint64_t)argv,
                               (uint64_t)(int64_t)in,
                               (uint64_t)(int64_t)out,
                               (uint64_t)(int64_t)err,
                               0);
}

int64_t os64_wait(int64_t pid, int32_t *exit_code)
{
    return (long)os64_syscall2(SYSCALL_WAIT, (uint64_t)pid, (uint64_t)exit_code);
}
