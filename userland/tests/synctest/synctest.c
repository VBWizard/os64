// synctest — ring-3 fixture for SYSCALL_SYNC_ALL (the sync(1) engine).
//
// Recreates the discovery that motivated the whole slice (2026-08-06): a
// program writes a file and holds it open — on FAT, the file's true length
// lives only in the writer's FIL, so every fresh open (a stat, a cat) reads
// the STALE directory entry until somebody syncs. sync_all is the somebody.
//
// The assertions are deliberately environment-honest: whether the first
// stat sees a stale length depends on the filesystem under /home (FAT
// defers, ext2's write-through doesn't), so staleness is OBSERVED but not
// required. The contract under test is the part that must hold everywhere:
// AFTER os64_sync_all(), a fresh stat reports the true length — even though
// the writer never closed and this process doesn't hold the writing handle
// anymore than `sync` at a shell would.
//
// Exit codes 0x05CC00xx ("05 CC" — the broom's badge; step in the low byte):
//   0x05CC0000  success
//   0x05CC0001  open-for-write failed (kernel test treats as SKIP —
//               the boot has no writable mount at /home)
//   0x05CC0002  write came up short
//   0x05CC0003  first stat failed
//   0x05CC0005  os64_sync_all() reported failure
//   0x05CC0006  second stat failed
//   0x05CC0007  post-sync length is still wrong — THE failing case

#include "os64/os64.h"

#define SYNCTEST_PATH "/home/synctest.tmp"

static const char kPayload[] = "the broom sweeps for whoever is holding the door open\n";
#define PAYLOAD_LEN ((int64_t)(sizeof(kPayload) - 1))

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    int32_t fd = (int32_t)os64_open(SYNCTEST_PATH, "w");
    if (fd < 0)
        return 0x05CC0001;

    if (os64_write(fd, kPayload, (size_t)PAYLOAD_LEN) != PAYLOAD_LEN)
    {
        os64_close(fd);
        return 0x05CC0002;
    }

    // Deliberately NO close and NO per-handle sync: fd now plays the role
    // of ping's still-open log handle.
    os64_dirent_t before = {0};
    if (os64_stat(SYNCTEST_PATH, &before) < 0)
    {
        os64_close(fd);
        return 0x05CC0003;
    }

    if (os64_sync_all() < 0)
    {
        os64_close(fd);
        return 0x05CC0005;
    }

    os64_dirent_t after = {0};
    if (os64_stat(SYNCTEST_PATH, &after) < 0)
    {
        os64_close(fd);
        return 0x05CC0006;
    }

    // The contract: post-sync, the directory entry tells the truth.
    // (before.size is reported for the curious via the exit path only in
    // spirit — the kernel test prints both sizes from its side if this
    // fails.)
    int64_t rc = ((int64_t)after.size == PAYLOAD_LEN) ? 0x05CC0000 : 0x05CC0007;

    os64_close(fd);
    os64_unlink(SYNCTEST_PATH);   // best-effort tidy; a leftover tmp is not a failure
    return (int)rc;
}
