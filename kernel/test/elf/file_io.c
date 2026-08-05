// file_io.c — ring-3 fixture for the file-handle syscalls: open, read, seek,
// close. Runs at CPL 3 like syscall_smoke, and like it, exits with a distinct
// 0xF11Exxxx code per failed step so a regression names its own culprit.
//
// Unlike the older fixtures, this one takes its syscall numbers AND raw stubs
// straight from the abi contract (<os64/syscall.h>) — no hand-copied defines
// to drift. If this fixture won't compile, the ABI changed and the fixture
// deserves to notice.
//
// The victim is /bin/hello — guaranteed on every image (the build discovers
// userland apps onto the disk automatically), read-only, and it starts with
// the four bytes everyone knows: 0x7f 'E' 'L' 'F'. That gives us known
// content at known offsets without putting a test data file on the image.
//
// Steps, in dependency order:
//   1. open("/bin/hello", "r")       -> handle >= 3 (0/1/2 are never displaced)
//   2. read 4 bytes                  -> exactly 4, equal to \x7fELF
//   3. seek(+1 from start)           -> returns 1 (seek reports NEW position)
//   4. read 3 bytes                  -> "ELF" (position actually moved)
//   5. seek(0 from end)              -> returns the file size, which is > 4
//   6. open("/no/such/file", "r")    -> negative (failure is in-band, no handle
//                                       burned; close(h) still works after)
//   7. close(h)                      -> 0
//   8. close(h) again                -> negative (the slot really was freed —
//                                       double-close is an error, not a nop)

#include <stdint.h>
#include "os64/syscall.h"

#define FILE_IO_OK         0x0F11E60DUL   // "FILE GOOD" — all checks passed
#define FAIL_OPEN          0xF11E0001UL   // step 1: open failed / bad handle
#define FAIL_READ_MAGIC    0xF11E0002UL   // step 2: wrong count or bytes
#define FAIL_SEEK_SET      0xF11E0003UL   // step 3: seek didn't return 1
#define FAIL_READ_AFTER    0xF11E0004UL   // step 4: post-seek read wrong
#define FAIL_SEEK_END      0xF11E0005UL   // step 5: size not sane
#define FAIL_OPEN_BOGUS    0xF11E0006UL   // step 6: nonexistent path "opened"
#define FAIL_CLOSE         0xF11E0007UL   // step 7: close failed
#define FAIL_DOUBLE_CLOSE  0xF11E0008UL   // step 8: second close "succeeded"

// Syscall results are in-band: the kernel's failure sentinels are huge
// unsigned values, so "negative as int64" is the error test.
static inline int failed(uint64_t v) { return (int64_t)v < 0; }

static void __attribute__((noreturn)) exit_with(uint64_t code)
{
    os64_syscall1(SYSCALL_EXIT, code);
    __builtin_unreachable();
}

unsigned long _start(unsigned long argc, char **argv, char **env)
{
    (void)argc; (void)argv; (void)env;

    char buf[8];

    // 1. Open something that must exist.
    uint64_t h = os64_syscall2(SYSCALL_OPEN, (uint64_t)"/bin/hello", (uint64_t)"r");
    if (failed(h) || h < 3)
        exit_with(FAIL_OPEN);

    // 2. The four most famous bytes in systems programming. Since the
    //    read-patience ruling (2026-08-05), arg3 is the timeout and every
    //    caller states it out loud — a raw 3-arg read leaves r10 garbage,
    //    which the boundary now (correctly) refuses as a random patience.
    uint64_t n = os64_syscall4(SYSCALL_READ, h, (uint64_t)buf, 4, OS64_WAIT_FOREVER);
    if (n != 4 || buf[0] != 0x7f || buf[1] != 'E' || buf[2] != 'L' || buf[3] != 'F')
        exit_with(FAIL_READ_MAGIC);

    // 3. Seek reports the NEW absolute position — 1, not 0, not the old spot.
    if (os64_syscall3(SYSCALL_SEEK, h, 1, OS64_SEEK_SET) != 1)
        exit_with(FAIL_SEEK_SET);

    // 4. And the position must have actually moved: "ELF" without the 0x7f.
    n = os64_syscall4(SYSCALL_READ, h, (uint64_t)buf, 3, OS64_WAIT_FOREVER);
    if (n != 3 || buf[0] != 'E' || buf[1] != 'L' || buf[2] != 'F')
        exit_with(FAIL_READ_AFTER);

    // 5. SEEK_END with offset 0 is "tell me the size" — an ELF with a program
    //    header can't plausibly be 4 bytes, so size > 4 is the sanity line.
    uint64_t size = os64_syscall3(SYSCALL_SEEK, h, 0, OS64_SEEK_END);
    if (failed(size) || size <= 4)
        exit_with(FAIL_SEEK_END);

    // 6. A nonexistent path must fail in-band — and must NOT have disturbed
    //    the handle we already hold (steps 7/8 prove h is still live/dead
    //    exactly when it should be).
    if (!failed(os64_syscall2(SYSCALL_OPEN, (uint64_t)"/no/such/file", (uint64_t)"r")))
        exit_with(FAIL_OPEN_BOGUS);

    // 7. Close releases the slot and the underlying file...
    if (failed(os64_syscall1(SYSCALL_CLOSE, h)))
        exit_with(FAIL_CLOSE);

    // 8. ...really releases it: closing a dead handle is an ERROR. If this
    //    "succeeds", the table just closed somebody else's future handle.
    if (!failed(os64_syscall1(SYSCALL_CLOSE, h)))
        exit_with(FAIL_DOUBLE_CLOSE);

    exit_with(FILE_IO_OK);
}
