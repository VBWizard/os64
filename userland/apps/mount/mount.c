// mount.c — put a partition into the namespace, or show what is in it.
//
//   mount                 print the mount table (/sys/mounts, verbatim)
//   mount WHAT            mount partition WHAT at its own GPT label ("/fat")
//   mount WHAT WHERE      mount partition WHAT at path WHERE
//   mount -r ...          either form, read-only: every write verb refused
//
// WHAT names the PARTITION: its GPT name ("home") or its dashed GUID — never
// a device path. The name travels with the disk; a device path describes one
// machine on one boot. WHERE's parent must exist; WHERE itself need not —
// the mount table is the namespace at that level. No WHERE and the kernel
// derives it from the label, exactly as a one-token mounts.conf line does —
// `mount fat` and `mount = fat` are the same ask through the same door.
// The kernel's refusals come back as os64/mount.h codes, and each gets its
// own sentence here, because "busy" and "no such partition" demand
// different next moves.
//
// Boot-time mounts are this verb's sibling: /etc/mounts.conf lines say what
// joins the namespace when the machine comes up, through the same kernel door.

#include "os64/os64.h"
#include "os64/mount.h"

static const char *mount_strerror(int64_t code)
{
    switch (code)
    {
        case OS64_MOUNT_BAD_ARGS:        return "bad arguments";
        case OS64_MOUNT_NOT_FOUND:       return "no partition wears that name or GUID (see /sys/block)";
        case OS64_MOUNT_ALREADY_MOUNTED: return "that partition is already mounted (see /sys/mounts)";
        case OS64_MOUNT_PREFIX_IN_USE:   return "something is already mounted there";
        case OS64_MOUNT_NO_PARENT:       return "the mount point's parent is not an existing directory";
        case OS64_MOUNT_TABLE_FULL:      return "the mount table is full";
        case OS64_MOUNT_UNSUPPORTED:     return "no filesystem os64 recognizes on it";
        case OS64_MOUNT_FAILED:          return "the filesystem driver refused it";
        case OS64_MOUNT_NO_FAT_VOLUME:   return "every FAT volume number is in use — unmount a FAT mount first";
        default:                         return "failed";
    }
}

static int print_mount_table(void)
{
    int64_t h = os64_open("/sys/mounts", "r");
    if (h < 0)
    {
        os64_hprintf(OS64_STDERR, "mount: cannot open /sys/mounts\n");
        return 1;
    }
    char buf[512];
    int64_t n;
    while ((n = os64_read((int32_t)h, buf, sizeof(buf))) > 0)
        os64_write(OS64_STDOUT, buf, (size_t)n);
    os64_close((int32_t)h);
    return 0;
}

int main(int argc, char **argv)
{
    bool readonly = false;
    const os64_optspec_t specs[] = {
        {'r', "readonly", false, "mount read-only (writes refused from birth)",
         .flag = &readonly}
    };
    os64_args_t args = {0};
    const char *operands[2] = {0};

    os64_args_init(&args, argc, argv, specs, 1);
    args.about = "Mount a partition, or list what is mounted.";
    args.details = "WHAT is a GPT partition name or GUID (/sys/block lists them);\n"
                   "WHERE is the mount point — omitted, the partition mounts at\n"
                   "its own GPT label. With no operands, prints /sys/mounts.\n"
                   "Boot-time mounts come from /etc/mounts.conf, through the same door.";

    int32_t count = os64_args_parse(&args, "mount [-r] [WHAT [WHERE]]", operands, 2);
    if (count == OS64_ARG_HELP)
        return 0;
    if (count < 0)
        return 2;

    if (count == 0)
        return print_mount_table();

    int64_t r = os64_mount(operands[0], (count == 2) ? operands[1] : NULL,
                           readonly ? OS64_MOUNT_RO : 0);
    if (r != OS64_MOUNT_OK)
    {
        os64_hprintf(OS64_STDERR, "mount: %s: %s\n", operands[0], mount_strerror(r));
        return 1;
    }
    return 0;
}
