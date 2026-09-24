// closetest — a close that fails is heard in ring 3.
//
// Three answers a close can give, and each has to be told apart from the
// others (os64/file.h): 0 for a handle dropped with nothing left undone,
// OS64_CLOSE_NOT_COMMITTED for a handle dropped whose bytes did not land
// (a flush that failed; a file that judges what it was given and refused),
// and the boundary's -1 for a handle that was never there. Until 2026-09-22
// the middle answer did not exist: `cp bad.psf /sys/console/font` exited 0.
//
// The refusing file used here is /sys/console/font, which judges a font at
// close and refuses one that is not PSF2 — the case that found the debt.
// Exit codes name the step that failed (0xC10E....: "CLOSE").

#include "os64/os64.h"
#include "os64/file.h"

#define BADGE 0xC10E0000u

int main(void)
{
    // 1. A file that refuses at close answers NOT_COMMITTED — not 0, and not
    //    "bad handle", because the handle WAS there and is gone now.
    int64_t h = os64_open("/sys/console/font", "w");
    if (h < 0)
        return BADGE + 1;
    static const char junk[] = "this is not a font";
    if (os64_write((int32_t)h, junk, sizeof(junk) - 1) != (int64_t)(sizeof(junk) - 1))
        return BADGE + 2;
    int64_t r = os64_close((int32_t)h);
    os64_printf("closetest: close of a refused font answered %ld (want %d)\n", (long)r, OS64_CLOSE_NOT_COMMITTED);
    if (r != OS64_CLOSE_NOT_COMMITTED)
        return BADGE + 3;

    // 2. And the handle really is gone: closing it again is the boundary's
    //    "no such handle", not a second refusal.
    r = os64_close((int32_t)h);
    os64_printf("closetest: closing it again answered %ld (want %d)\n", (long)r, OS64_FILE_ERR_INVALID);
    if (r != OS64_FILE_ERR_INVALID)
        return BADGE + 4;

    // 3. An ordinary file that commits answers 0 (ext2 root: write-through,
    //    so the close has nothing left to fail at).
    h = os64_open("/tmp/closetest.txt", "w");
    if (h < 0)
        return BADGE + 5;
    if (os64_write((int32_t)h, "ok\n", 3) != 3)
        return BADGE + 6;
    r = os64_close((int32_t)h);
    os64_printf("closetest: close of a committed file answered %ld (want 0)\n", (long)r);
    if (r != 0)
        return BADGE + 7;
    (void)os64_unlink("/tmp/closetest.txt");

    // 4. A handle that never existed is the boundary's answer, not the
    //    family's.
    r = os64_close(1234);
    os64_printf("closetest: closing a handle that never was answered %ld (want %d)\n", (long)r, OS64_FILE_ERR_INVALID);
    if (r != OS64_FILE_ERR_INVALID)
        return BADGE + 8;

    // 5. A spawn that FAILS gives its redirection share back whole. The
    //    share is held as an operation in flight until a child owns it, and
    //    a failed load releases it as one; if either half were wrong the
    //    close below would answer "deferred" (a pin left behind) or the
    //    file would already be gone. What must come back is the refusal —
    //    this handle is the only holder again, and it answers for the file.
    h = os64_open("/sys/console/font", "w");
    if (h < 0)
        return BADGE + 9;
    char *const noargs[] = { "nonesuch", NULL };
    int64_t pid = os64_spawn_redirected("/tests/nonesuch", noargs, -1, (int32_t)h, -1, 0);
    os64_printf("closetest: spawning a program that does not exist answered %ld (want < 0)\n", (long)pid);
    if (pid >= 0)
        return BADGE + 10;
    if (os64_write((int32_t)h, junk, sizeof(junk) - 1) != (int64_t)(sizeof(junk) - 1))
        return BADGE + 11;
    r = os64_close((int32_t)h);
    os64_printf("closetest: close after a failed spawn answered %ld (want %d)\n", (long)r, OS64_CLOSE_NOT_COMMITTED);
    if (r != OS64_CLOSE_NOT_COMMITTED)
        return BADGE + 12;

    // 6. A spawn that SUCCEEDS turns the share into the child's handle, so
    //    the parent's close answers 0 — that copy will answer for the file
    //    — and not "deferred", which is what a share still counted as an
    //    operation would make it say.
    h = os64_open("/tmp/closetest.txt", "w");
    if (h < 0)
        return BADGE + 13;
    char *const trueargs[] = { "true", NULL };
    pid = os64_spawn_redirected("/bin/true", trueargs, -1, (int32_t)h, -1, 0);
    if (pid < 0)
        return BADGE + 14;
    r = os64_close((int32_t)h);
    os64_printf("closetest: close of a file a child also holds answered %ld (want 0)\n", (long)r);
    int32_t code = -1;
    while (os64_wait(pid, &code) == OS64_INTERRUPTED)
        ;
    (void)os64_unlink("/tmp/closetest.txt");
    if (r != 0)
        return BADGE + 15;

    return BADGE;
}
