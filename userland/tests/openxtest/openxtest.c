// openxtest — ring-3 proof for open(path, "x").
//
// The mode is useful only if the whole promise survives contention and
// resource failure: one creator wins an absent name, an existing file is
// never truncated, and a full caller handle table creates nothing. The parent
// runs those checks against ext2 and FAT in either root layout, then launches
// two copies of itself through one pipe barrier to contend for each name.

#include "os64/os64.h"

#define PASS_CODE       0x0EAC7000
#define CHILD_WON       0x0EAC7010
#define CHILD_REFUSED   0x0EAC7011

static const char kFirst[] = "first-writer";

static bool directory_exists(const char *path)
{
    os64_dirent_t entry;
    return os64_stat(path, &entry) == 0 && (entry.flags & OS64_DE_DIR);
}

static bool missing(const char *path)
{
    os64_dirent_t entry;
    return os64_stat(path, &entry) < 0;
}

static bool holds(const char *path, const char *bytes, size_t length)
{
    char buf[32];
    int32_t fd = (int32_t)os64_open(path, "r");
    if (fd < 0)
        return false;
    int64_t got = os64_read(fd, buf, sizeof(buf));
    bool closed = os64_close(fd) == 0;
    if (!closed || got != (int64_t)length)
        return false;
    for (size_t i = 0; i < length; i++)
        if (buf[i] != bytes[i])
            return false;
    return true;
}

static int child_claim(const char *path)
{
    char go;
    if (os64_read(0, &go, 1) != 1)
        return 0x0EAC70F0;

    int32_t fd = (int32_t)os64_open(path, "x");
    if (fd < 0)
        return CHILD_REFUSED;
    bool ok = os64_write(fd, "W", 1) == 1 && os64_close(fd) == 0;
    return ok ? CHILD_WON : 0x0EAC70F1;
}

static bool race_claim(const char *path)
{
    os64_unlink(path);

    int32_t barrier[2];
    if (os64_pipe(barrier) < 0)
        return false;
    char *const argv[] = { "/tests/openxtest", "claim", (char *)path, NULL };
    int64_t a = os64_spawn_redirected(argv[0], argv, barrier[0], -1, -1, 0);
    int64_t b = os64_spawn_redirected(argv[0], argv, barrier[0], -1, -1, 0);
    os64_close(barrier[0]);
    if (a < 0 || b < 0)
    {
        os64_close(barrier[1]);
        return false;
    }

    bool released = os64_write(barrier[1], "GG", 2) == 2;
    os64_close(barrier[1]);
    int32_t ac = -1, bc = -1;
    bool waited = os64_wait(a, &ac) == a && os64_wait(b, &bc) == b;
    bool one_winner = ((uint32_t)ac == CHILD_WON &&
                       (uint32_t)bc == CHILD_REFUSED) ||
                      ((uint32_t)bc == CHILD_WON &&
                       (uint32_t)ac == CHILD_REFUSED);
    bool ok = released && waited && one_winner && holds(path, "W", 1);
    bool removed = os64_unlink(path) == 0 && missing(path);
    return ok && removed;
}

static bool handle_full_creates_nothing(const char *path)
{
    os64_unlink(path);
    int32_t held[13];
    int count = 0;
    while (count < 13)
    {
        held[count] = (int32_t)os64_open("/bin/hello", "r");
        if (held[count] < 0)
            break;
        count++;
    }

    int64_t refused = os64_open(path, "x");
    bool ok = count == 13 && refused < 0 && missing(path);
    if (refused >= 0)
        os64_close((int32_t)refused);
    while (count > 0)
        os64_close(held[--count]);
    os64_unlink(path);
    return ok;
}

static bool exercise(const char *path)
{
    os64_unlink(path);

    int32_t fd = (int32_t)os64_open(path, "x");
    if (fd < 0)
        return false;
    bool wrote = os64_write(fd, kFirst, sizeof(kFirst) - 1) ==
                 (int64_t)(sizeof(kFirst) - 1);

    // The second open must refuse even while the winner still holds the file.
    int64_t second = os64_open(path, "x");
    if (second >= 0)
        os64_close((int32_t)second);
    bool closed = os64_close(fd) == 0;
    bool preserved = holds(path, kFirst, sizeof(kFirst) - 1);
    bool removed = os64_unlink(path) == 0 && missing(path);

    if (!wrote || second >= 0 || !closed || !preserved || !removed)
        return false;
    if (!handle_full_creates_nothing(path))
        return false;
    return race_claim(path);
}

int main(int argc, char **argv)
{
    if (argc == 3 && os64_streq(argv[1], "claim"))
        return child_claim(argv[2]);

    bool fat_is_secondary = directory_exists("/fat");
    bool ext2_is_secondary = directory_exists("/ext2");
    if (fat_is_secondary == ext2_is_secondary)
        return 0x0EAC7001;

    const char *ext2_path = fat_is_secondary ? "/openxtest.ext2"
                                              : "/ext2/openxtest.ext2";
    const char *fat_path = fat_is_secondary ? "/fat/openxtest.fat"
                                             : "/openxtest.fat";

    if (!exercise(ext2_path))
        return 0x0EAC7002;
    if (!exercise(fat_path))
        return 0x0EAC7003;

    // The new letter is exact, not a prefix, and synthetic filesystems do
    // not acquire namespace mutation merely because the syscall knows it.
    int64_t invalid = os64_open("/openxtest.invalid", "xx");
    int64_t synthetic = os64_open("/proc/openxtest", "x");
    if (invalid >= 0)
        os64_close((int32_t)invalid);
    if (synthetic >= 0)
        os64_close((int32_t)synthetic);
    if (invalid >= 0 || synthetic >= 0 || !missing("/openxtest.invalid"))
        return 0x0EAC7004;

    return PASS_CODE;
}
