// redirect_io.c — ring-3 fixture for FILE REDIRECTION through spawn: the
// kernel mechanics under husk's `prog < in > out`, minus husk.
//
// The dangerous sequence this exists to prove safe: open a file, hand it to
// a child's stdout via spawn, and close OUR handle immediately — while the
// child is still writing through its copy. Before vfs_file_t grew
// handleRefCount, that close ran fops->close and freed the FIL out from
// under the child (use-after-free with a side of disk corruption). The
// refcount makes death-by-parent impossible: the child's slot is a second
// reference, and only the LAST close closes.
//
// Steps (0x2ED1xxxx codes name the failed step):
//   1. open /redir_t.txt "w"            -> handle
//   2. spawn /bin/hello with out=that   -> pid (hello's stdout is the FILE)
//   3. close OUR copy BEFORE waiting    -> must succeed; child unaffected
//   4. wait(pid)                        -> hello exits 0
//   5. open /redir_t.txt "r", read      -> starts with "Hello" (bytes really
//                                          landed on disk through the child)
//   6. spawn /bin/upper with in=/redir_t.txt, out=/redir_t2.txt, close both
//      copies, wait                     -> upper filters FILE to FILE
//   7. read /redir_t2.txt               -> starts with "HELLO" (both slots
//                                          redirected at once, both worked)

#include <stdint.h>
#include "os64/syscall.h"

#define REDIR_OK           0x2ED1600DUL   // all checks passed
#define FAIL_OPEN_OUT      0x2ED10001UL   // step 1
#define FAIL_SPAWN_HELLO   0x2ED10002UL   // step 2
#define FAIL_EARLY_CLOSE   0x2ED10003UL   // step 3
#define FAIL_WAIT_HELLO    0x2ED10004UL   // step 4
#define FAIL_READ_BACK     0x2ED10005UL   // step 5 (open or read)
#define FAIL_CONTENT       0x2ED10006UL   // step 5 (bytes wrong)
#define FAIL_UPPER_CHAIN   0x2ED10007UL   // step 6 (any part)
#define FAIL_UPPER_OUTPUT  0x2ED10008UL   // step 7

static inline int failed(uint64_t v) { return (int64_t)v < 0; }

static void __attribute__((noreturn)) exit_with(uint64_t code)
{
    os64_syscall1(SYSCALL_EXIT, code);
    __builtin_unreachable();
}

unsigned long _start(unsigned long argc, char **argv, char **env)
{
    (void)argc; (void)argv; (void)env;

    char buf[16];
    int wcode = 0;

    // 1-2. hello's stdout becomes a file it will never know about.
    uint64_t hOut = os64_syscall2(SYSCALL_OPEN, (uint64_t)"/redir_t.txt", (uint64_t)"w");
    if (failed(hOut))
        exit_with(FAIL_OPEN_OUT);

    uint64_t pid = os64_syscall6(SYSCALL_SPAWN, (uint64_t)"/bin/hello", 0,
                                 (uint64_t)-1, hOut, (uint64_t)-1, 0);
    if (failed(pid))
        exit_with(FAIL_SPAWN_HELLO);

    // 3. THE test: drop our reference while the child still holds its own.
    if (failed(os64_syscall1(SYSCALL_CLOSE, hOut)))
        exit_with(FAIL_EARLY_CLOSE);

    // 4. If the refcount is broken, hello is now writing through freed
    //    memory and this wait ends in tears (or never ends).
    if (failed(os64_syscall2(SYSCALL_WAIT, pid, (uint64_t)&wcode)) || wcode != 0)
        exit_with(FAIL_WAIT_HELLO);

    // 5. The child's bytes must be ON THE DISK, readable by a fresh open.
    uint64_t hIn = os64_syscall2(SYSCALL_OPEN, (uint64_t)"/redir_t.txt", (uint64_t)"r");
    if (failed(hIn))
        exit_with(FAIL_READ_BACK);
    // (syscall4 with trailing 0: read's arg3 = deadline, 0 = block forever)
    if (os64_syscall4(SYSCALL_READ, hIn, (uint64_t)buf, 5, 0) != 5)
        exit_with(FAIL_READ_BACK);
    os64_syscall1(SYSCALL_CLOSE, hIn);
    if (buf[0] != 'H' || buf[1] != 'e' || buf[2] != 'l' || buf[3] != 'l' || buf[4] != 'o')
        exit_with(FAIL_CONTENT);

    // 6. Both slots at once: upper reads the file, writes the other file —
    //    the full `upper < in > out` shape, no console anywhere.
    uint64_t uIn  = os64_syscall2(SYSCALL_OPEN, (uint64_t)"/redir_t.txt", (uint64_t)"r");
    uint64_t uOut = os64_syscall2(SYSCALL_OPEN, (uint64_t)"/redir_t2.txt", (uint64_t)"w");
    if (failed(uIn) || failed(uOut))
        exit_with(FAIL_UPPER_CHAIN);
    pid = os64_syscall6(SYSCALL_SPAWN, (uint64_t)"/bin/upper", 0,
                        uIn, uOut, (uint64_t)-1, 0);
    if (failed(pid))
        exit_with(FAIL_UPPER_CHAIN);
    os64_syscall1(SYSCALL_CLOSE, uIn);    // the close discipline, file edition:
    os64_syscall1(SYSCALL_CLOSE, uOut);   // ours go away, the child's survive
    if (failed(os64_syscall2(SYSCALL_WAIT, pid, (uint64_t)&wcode)) || wcode != 0)
        exit_with(FAIL_UPPER_CHAIN);

    // 7. And the transform must have landed: HELLO, at file offset 0.
    hIn = os64_syscall2(SYSCALL_OPEN, (uint64_t)"/redir_t2.txt", (uint64_t)"r");
    if (failed(hIn))
        exit_with(FAIL_UPPER_OUTPUT);
    if (os64_syscall4(SYSCALL_READ, hIn, (uint64_t)buf, 5, 0) != 5)
        exit_with(FAIL_UPPER_OUTPUT);
    os64_syscall1(SYSCALL_CLOSE, hIn);
    if (buf[0] != 'H' || buf[1] != 'E' || buf[2] != 'L' || buf[3] != 'L' || buf[4] != 'O')
        exit_with(FAIL_UPPER_OUTPUT);

    exit_with(REDIR_OK);
}
