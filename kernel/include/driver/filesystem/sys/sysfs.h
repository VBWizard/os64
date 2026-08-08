#ifndef SYSFS_H
#define SYSFS_H

// /sys — the machine as files. See the header comment in sysfs.c for the
// namespace and the doctrine. Mounted from kernel_init, right after /proc.
void sysfs_mount(void);

#endif
