#ifndef OS64_MOUNT_H
#define OS64_MOUNT_H

// The mount/unmount result vocabulary — shared by the kernel's vfs and the
// programs that call os64_mount/os64_unmount, because the WHY is the whole
// point of refusing: "busy" and "no such partition" demand different next
// moves from the person at the shell. os64 has no errno; a call's result
// carries its own meaning (DIVERGENCES § Syscalls).
//
// `what` names a PARTITION: its GPT name ("home", "ext2") or its unique
// GUID in the usual dashed spelling. Never a device path — a name travels
// with the disk, a path describes one machine on one boot (PARTLABEL is
// where Linux ended up; os64 starts there).
//
// `where` is an absolute path. The mount point itself need not exist — the
// mount table IS the namespace at that level (Plan 9's view, ratified
// 2026-08-30) — but its PARENT must be a real directory (or "/"), so `ls`
// never has to invent one. NULL (or "") mounts the partition at its own GPT
// label — `mount fat` lands at /fat, the same derivation a one-token
// mounts.conf line gets.
//
// `flags` — bits below; any bit outside them is refused as BAD_ARGS, so a
// program built against a newer vocabulary fails loudly on an older kernel
// instead of being silently granted less than it asked for.

#define OS64_MOUNT_RO             0x1   // mount read-only: every write verb refused from birth

#define OS64_MOUNT_OK               0
#define OS64_MOUNT_BAD_ARGS        -1   // NULL/overlong/relative arguments, or an unknown flag bit
#define OS64_MOUNT_NOT_FOUND       -2   // no partition wears that name or GUID
                                        // -3 is retired (the foreign refusal; mounts.conf made
                                        // naming a partition the deliberateness) — never reuse it
#define OS64_MOUNT_ALREADY_MOUNTED -4   // that partition is in the namespace already
#define OS64_MOUNT_PREFIX_IN_USE   -5   // something else is mounted at `where`
#define OS64_MOUNT_NO_PARENT       -6   // `where`'s parent is not an existing directory
#define OS64_MOUNT_TABLE_FULL      -7   // VFS_MAX_MOUNTS reached
#define OS64_MOUNT_UNSUPPORTED     -8   // no filesystem os64 recognizes on it
#define OS64_MOUNT_FAILED          -9   // the driver refused it (unreadable superblock...)

#define OS64_UNMOUNT_NOT_A_MOUNT  -10   // `where` is not a mount point
#define OS64_UNMOUNT_ROOT         -11   // "/" is not unmountable, ever
#define OS64_UNMOUNT_SYNTH        -12   // /proc, /sys, /dev — not disk mounts
#define OS64_UNMOUNT_BUSY         -13   // open files/directories on it, or a task's cwd inside it

// Numbers are handed out in the order the refusals were discovered, so the
// mount and unmount runs stopped being contiguous here. The VALUE is the
// ABI; the grouping never was.
#define OS64_MOUNT_NO_FAT_VOLUME  -14   // FatFs volume numbers all spoken for (unmount a FAT mount)
#define OS64_UNMOUNT_HAS_MOUNTS   -15   // something is mounted UNDER `where` — unmount that first

#endif
