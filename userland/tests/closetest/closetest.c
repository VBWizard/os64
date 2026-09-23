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

    return BADGE;
}
