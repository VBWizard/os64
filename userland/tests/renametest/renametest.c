// renametest — ring-3 proof for rename's opt-in destination policies.
//
// The old os64_rename() call remains in this fixture on purpose. A raw syscall
// 43 carrying nonzero register residue proves its two-argument ABI stayed
// intact; the flagged calls prove the new syscall reaches the filesystem and
// that invalid policies fail before either name changes.

#include "os64/os64.h"

#define ROOT_SOURCE_PATH       "/renametest.source"
#define ROOT_DEST_PATH         "/renametest.dest"
#define FAT_SOURCE_PATH        "/fat/renametest.source"
#define FAT_DEST_PATH          "/fat/renametest.dest"
#define EXT2_SOURCE_PATH       "/ext2/renametest.source"
#define EXT2_DEST_PATH         "/ext2/renametest.dest"

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

static bool directory_exists(const char *path)
{
    os64_dirent_t de;
    return os64_stat(path, &de) == 0 && (de.flags & OS64_DE_DIR);
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

    // The secondary partition names the root's opposite: /fat exists when
    // ext2 is root, while /ext2 exists on the FAT-root lifeboat. Exercise the
    // same two filesystems in either layout rather than assigning semantics
    // to a hard-coded root path.
    bool fat_is_secondary = directory_exists("/fat");
    bool ext2_is_secondary = directory_exists("/ext2");
    if (fat_is_secondary == ext2_is_secondary)
        return 0x4E4D0008;

    const char *ext2_source = fat_is_secondary ? ROOT_SOURCE_PATH
                                                : EXT2_SOURCE_PATH;
    const char *ext2_dest = fat_is_secondary ? ROOT_DEST_PATH
                                              : EXT2_DEST_PATH;
    const char *fat_source = fat_is_secondary ? FAT_SOURCE_PATH
                                               : ROOT_SOURCE_PATH;
    const char *fat_dest = fat_is_secondary ? FAT_DEST_PATH
                                             : ROOT_DEST_PATH;

    tidy_pair(ext2_source, ext2_dest);
    if (!plant(ext2_source, kSource, sizeof(kSource) - 1) ||
        !plant(ext2_dest, kDest, sizeof(kDest) - 1)) {
        rc = 0x4E4D0001;
        goto out;
    }

    if (os64_rename_with_flags(ext2_source, ext2_dest,
                               OS64_RENAME_NOREPLACE) == 0 ||
        !holds(ext2_source, kSource, sizeof(kSource) - 1) ||
        !holds(ext2_dest, kDest, sizeof(kDest) - 1)) {
        rc = 0x4E4D0002;
        goto out;
    }

    if (os64_rename_with_flags(ext2_source, ext2_dest, 0x80) == 0 ||
        os64_rename_with_flags(ext2_source, ext2_dest,
                               OS64_RENAME_FLAG_MASK) == 0 ||
        !holds(ext2_source, kSource, sizeof(kSource) - 1) ||
        !holds(ext2_dest, kDest, sizeof(kDest) - 1)) {
        rc = 0x4E4D0003;
        goto out;
    }

    if (os64_rename_with_flags(ext2_source, ext2_dest,
                               OS64_RENAME_REQUIRE_ATOMIC_REPLACE) != 0 ||
        !missing(ext2_source) ||
        !holds(ext2_dest, kSource, sizeof(kSource) - 1)) {
        rc = 0x4E4D0004;
        goto out;
    }

    if (!plant(ext2_source, kLegacy, sizeof(kLegacy) - 1) ||
        os64_rename(ext2_source, ext2_dest) != 0 ||
        !holds(ext2_dest, kLegacy, sizeof(kLegacy) - 1)) {
        rc = 0x4E4D0005;
        goto out;
    }

    // Model an already-built raw caller with nonzero residue in the register
    // that became arg2. Syscall 43 must ignore it forever.
    if (!plant(ext2_source, kSource, sizeof(kSource) - 1) ||
        (int64_t)os64_syscall3(SYSCALL_RENAME, (uint64_t)ext2_source,
                               (uint64_t)ext2_dest, 0x80) != 0 ||
        !holds(ext2_dest, kSource, sizeof(kSource) - 1)) {
        rc = 0x4E4D0006;
        goto out;
    }

    if (!plant(ext2_source, kSource, sizeof(kSource) - 1) ||
        os64_unlink(ext2_dest) != 0 ||
        os64_rename_with_flags(ext2_source, ext2_dest,
                               OS64_RENAME_NOREPLACE) != 0 ||
        !holds(ext2_dest, kSource, sizeof(kSource) - 1))
        rc = 0x4E4D0007;

    // Both safe policies preserve an existing FAT destination, while the
    // legacy wrapper retains remove-then-rename.
    if (rc == 0x4E4D0000) {
        tidy_pair(fat_source, fat_dest);
        if (!plant(fat_source, kSource, sizeof(kSource) - 1) ||
            !plant(fat_dest, kDest, sizeof(kDest) - 1)) {
            rc = 0x4E4D0009;
            goto out;
        }
        if (os64_rename_with_flags(fat_source, fat_dest,
                                   OS64_RENAME_NOREPLACE) == 0 ||
            os64_rename_with_flags(fat_source, fat_dest,
                                   OS64_RENAME_REQUIRE_ATOMIC_REPLACE) == 0 ||
            !holds(fat_source, kSource, sizeof(kSource) - 1) ||
            !holds(fat_dest, kDest, sizeof(kDest) - 1)) {
            rc = 0x4E4D000A;
            goto out;
        }
        if (os64_rename(fat_source, fat_dest) != 0 ||
            !missing(fat_source) ||
            !holds(fat_dest, kSource, sizeof(kSource) - 1))
            rc = 0x4E4D000B;
    }

out:
    tidy_pair(ext2_source, ext2_dest);
    tidy_pair(fat_source, fat_dest);
    return rc;
}
