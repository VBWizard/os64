#ifndef PROCFS_H
#define PROCFS_H

// procfs.h — /proc, os64's first SYNTHETIC filesystem (design: PROC.md).
//
// Every other filesystem in the tree bottoms out in a block device: FAT and
// ext2 read sectors. This one bottoms out in the SCHEDULER. `ls /proc` walks
// kTaskList; `cat /proc/7/status` renders a task_t; `echo kill > /proc/7/ctl`
// raises a signal. There is no disk anywhere underneath it.
//
// The idea is Tom Killian's (UNIX 8th Edition, 1984 — "Processes as Files"),
// by way of Plan 9, which is where the per-process directory and the ctl file
// come from. os64 keeps Plan 9's discipline and rejects Linux's sprawl: if it
// is not a process, it does not live here. See PROC.md's constitution.

#include "driver/filesystem/vfs/vfs.h"

// Build the /proc filesystem and claim its prefix in the mount table. Call
// AFTER vfs_mount_root_part() — procfs carries an all-zero partition GUID
// (it has no partition), and mounting it after the auto-mount sweep keeps
// that zero out of the sweep's GUID-dedupe comparisons entirely.
void procfs_mount(void);

extern vfs_file_operations_t proc_fops;
extern vfs_directory_operations_t proc_dops;

#endif // PROCFS_H
