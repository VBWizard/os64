// renametest — ring-3 proof for rename's opt-in destination policies.
//
// The old os64_rename() call remains in this fixture on purpose. A raw syscall
// 43 carrying nonzero register residue proves its two-argument ABI stayed
// intact; the flagged calls prove the new syscall reaches the filesystem and
// that invalid policies fail before either name changes.

#include "os64/os64.h"

#define SOURCE_PATH "/tmp/renametest.source"
#define DEST_PATH   "/tmp/renametest.dest"
#define FAT_SOURCE_PATH "/fat/renametest.source"
#define FAT_DEST_PATH   "/fat/renametest.dest"

static const char kSource[] = "SOURCE";
static const char kDest[] = "DEST";
static const char kLegacy[] = "LEGACY";

static bool bytes_equal(const char *a, const char *b, size_t len)
{
    for (size_t i = 0; i < len; i++)
        if (a[i] != b[i])
            return false;
    return true;
}

static bool plant(const char *path, const char *bytes, size_t len)
{
    int32_t fd = (int32_t)os64_open(path, "w");
    if (fd < 0)
        return false;
    bool ok = os64_write(fd, bytes, len) == (int64_t)len;
    if (os64_close(fd) < 0)
        ok = false;
    return ok;
}

static bool holds(const char *path, const char *bytes, size_t len)
{
    char buf[16];
    int32_t fd = (int32_t)os64_open(path, "r");
    if (fd < 0)
        return false;
    int64_t n = os64_read(fd, buf, sizeof(buf));
    os64_close(fd);
    return n == (int64_t)len && bytes_equal(buf, bytes, len);
}

static bool missing(const char *path)
{
    os64_dirent_t de;
    return os64_stat(path, &de) < 0;
}

static void tidy_pair(const char *source, const char *dest)
{
    os64_unlink(source);
    os64_unlink(dest);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    int rc = 0x4E4D0000;

    tidy_pair(SOURCE_PATH, DEST_PATH);
    if (!plant(SOURCE_PATH, kSource, sizeof(kSource) - 1) ||
        !plant(DEST_PATH, kDest, sizeof(kDest) - 1)) {
        rc = 0x4E4D0001;
        goto out;
    }

    if (os64_rename_with_flags(SOURCE_PATH, DEST_PATH,
                               OS64_RENAME_NOREPLACE) == 0 ||
        !holds(SOURCE_PATH, kSource, sizeof(kSource) - 1) ||
        !holds(DEST_PATH, kDest, sizeof(kDest) - 1)) {
        rc = 0x4E4D0002;
        goto out;
    }

    if (os64_rename_with_flags(SOURCE_PATH, DEST_PATH, 0x80) == 0 ||
        os64_rename_with_flags(SOURCE_PATH, DEST_PATH,
                               OS64_RENAME_FLAG_MASK) == 0 ||
        !holds(SOURCE_PATH, kSource, sizeof(kSource) - 1) ||
        !holds(DEST_PATH, kDest, sizeof(kDest) - 1)) {
        rc = 0x4E4D0003;
        goto out;
    }

    if (os64_rename_with_flags(SOURCE_PATH, DEST_PATH,
                               OS64_RENAME_REQUIRE_ATOMIC_REPLACE) != 0 ||
        !missing(SOURCE_PATH) ||
        !holds(DEST_PATH, kSource, sizeof(kSource) - 1)) {
        rc = 0x4E4D0004;
        goto out;
    }

    if (!plant(SOURCE_PATH, kLegacy, sizeof(kLegacy) - 1) ||
        os64_rename(SOURCE_PATH, DEST_PATH) != 0 ||
        !holds(DEST_PATH, kLegacy, sizeof(kLegacy) - 1)) {
        rc = 0x4E4D0005;
        goto out;
    }

    // Model an already-built raw caller with nonzero residue in the register
    // that became arg2. Syscall 43 must ignore it forever.
    if (!plant(SOURCE_PATH, kSource, sizeof(kSource) - 1) ||
        (int64_t)os64_syscall3(SYSCALL_RENAME, (uint64_t)SOURCE_PATH,
                               (uint64_t)DEST_PATH, 0x80) != 0 ||
        !holds(DEST_PATH, kSource, sizeof(kSource) - 1)) {
        rc = 0x4E4D0006;
        goto out;
    }

    if (!plant(SOURCE_PATH, kSource, sizeof(kSource) - 1) ||
        os64_unlink(DEST_PATH) != 0 ||
        os64_rename_with_flags(SOURCE_PATH, DEST_PATH,
                               OS64_RENAME_NOREPLACE) != 0 ||
        !holds(DEST_PATH, kSource, sizeof(kSource) - 1))
        rc = 0x4E4D0007;

    // The ext2-root test boot mounts the FAT lifeboat at /fat. Exercise the
    // public syscall there too: both safe policies preserve an existing
    // destination, while the legacy wrapper retains remove-then-rename.
    os64_dirent_t fat;
    if (rc == 0x4E4D0000 &&
        (os64_stat("/fat", &fat) < 0 || !(fat.flags & OS64_DE_DIR))) {
        rc = 0x4E4D0008;
        goto out;
    }
    if (rc == 0x4E4D0000) {
        tidy_pair(FAT_SOURCE_PATH, FAT_DEST_PATH);
        if (!plant(FAT_SOURCE_PATH, kSource, sizeof(kSource) - 1) ||
            !plant(FAT_DEST_PATH, kDest, sizeof(kDest) - 1)) {
            rc = 0x4E4D0009;
            goto out;
        }
        if (os64_rename_with_flags(FAT_SOURCE_PATH, FAT_DEST_PATH,
                                   OS64_RENAME_NOREPLACE) == 0 ||
            os64_rename_with_flags(FAT_SOURCE_PATH, FAT_DEST_PATH,
                                   OS64_RENAME_REQUIRE_ATOMIC_REPLACE) == 0 ||
            !holds(FAT_SOURCE_PATH, kSource, sizeof(kSource) - 1) ||
            !holds(FAT_DEST_PATH, kDest, sizeof(kDest) - 1)) {
            rc = 0x4E4D000A;
            goto out;
        }
        if (os64_rename(FAT_SOURCE_PATH, FAT_DEST_PATH) != 0 ||
            !missing(FAT_SOURCE_PATH) ||
            !holds(FAT_DEST_PATH, kSource, sizeof(kSource) - 1))
            rc = 0x4E4D000B;
    }

out:
    tidy_pair(SOURCE_PATH, DEST_PATH);
    tidy_pair(FAT_SOURCE_PATH, FAT_DEST_PATH);
    return rc;
}
