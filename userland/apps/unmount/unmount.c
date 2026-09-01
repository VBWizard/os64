// unmount.c — take a mount out of the namespace.
//
//   unmount WHERE
//
// The verb is unmount because that is what it does — os64 never inherited
// the u-with-no-n Unix spelling (V1 lost the 'n' to a six-character symbol
// table limit in 1971, and everyone has typed around it since). The kernel
// refuses with an os64/mount.h code; BUSY names the arithmetic that held it
// down (/sys/mounts shows the open counts per mount).

#include "os64/os64.h"
#include "os64/mount.h"

static const char *unmount_strerror(int64_t code)
{
    switch (code)
    {
        case OS64_MOUNT_BAD_ARGS:       return "bad argument";
        case OS64_UNMOUNT_NOT_A_MOUNT:  return "not a mount point (see /sys/mounts)";
        case OS64_UNMOUNT_ROOT:         return "the root filesystem is not unmountable";
        case OS64_UNMOUNT_SYNTH:        return "that is the machine's own furniture, not a disk mount";
        case OS64_UNMOUNT_BUSY:         return "busy — open files or directories on it, or a task is cd'd into it";
        case OS64_UNMOUNT_HAS_MOUNTS:   return "something is mounted under it — unmount that first (see /sys/mounts)";
        default:                        return "failed";
    }
}

int main(int argc, char **argv)
{
    os64_args_t args = {0};
    const char *operand = NULL;

    os64_args_init(&args, argc, argv, NULL, 0);
    args.about = "Unmount a filesystem.";
    args.details = "WHERE is the mount point (/sys/mounts lists them).";

    int32_t count = os64_args_parse(&args, "unmount WHERE", &operand, 1);
    if (count == OS64_ARG_HELP)
        return 0;
    if (count < 0)
        return 2;
    if (count != 1)
    {
        os64_hprintf(OS64_STDERR, "unmount: which mount? (see /sys/mounts)\n");
        os64_args_help(&args, "unmount");
        return 2;
    }

    int64_t r = os64_unmount(operand);
    if (r != OS64_MOUNT_OK)
    {
        os64_hprintf(OS64_STDERR, "unmount: %s: %s\n", operand, unmount_strerror(r));
        return 1;
    }
    return 0;
}
