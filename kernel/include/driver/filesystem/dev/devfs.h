#ifndef DEVFS_H
#define DEVFS_H

// devfs — /dev: the kernel's OBJECTS as files.
//
// The third leg of os64's synthetic namespace, and the one SUCCESSION.md
// promised: "/bin, /lib, /etc, /home, /tmp, /dev later." Later is now.
//
//   /proc  renders PROCESSES   (procfs.c — Plan 9 style, processes only)
//   /sys   renders the MACHINE (sysfs.c — PCI, cores, cache, log, net)
//   /dev   IS the objects       (here)
//
// That last verb is the difference, and it is why this file cannot lean on
// synthfs's snapshot machinery the way its two older siblings do. /proc and
// /sys generate a whole text at open and serve it as an immutable snapshot —
// exactly right when the file is a REPORT, and exactly wrong when the file is
// a void that swallows everything or a faucet that never runs dry. So devfs
// is the first synthetic filesystem in os64 with LIVE fops: it borrows
// synthfs's mount dance and path helpers, and brings its own read and write.
//
// ── The residents ───────────────────────────────────────────────────────────
//
//   /dev/null   reads EOF, swallows every write.
//   /dev/zero   reads an endless run of zero bytes, swallows every write.
//   /dev/full   reads zeros like /dev/zero; every write FAILS.
//   /dev/tty    the caller's own terminal (see THE ALIAS below).
//
// ── Lineage, because it explains the shapes ─────────────────────────────────
//
// /dev/null is primordial — it is in the research Unix manuals by the early
// '70s, and it is the founding demonstration of the idea that made Unix Unix:
// special files live in the tree and are read and written like ordinary ones.
// That is the whole reason `cp` could work on a tape drive, and the whole
// reason `cmd > /dev/null` needs no shell magic here either.
//
// /dev/zero is a much later, System V-era arrival, and its real job was never
// `dd` — it was ANONYMOUS MEMORY. Before MAP_ANONYMOUS existed, you got
// zero-filled pages by mmap'ing /dev/zero; the file existed so that the void
// could be passed to a call that demanded a file. os64 never needed it for
// that (the VMA layer does anonymous zero pages natively, and every
// allocation is zeroed at the choke point), which is precisely why nobody
// here has missed it. What is left is its second career: a byte faucet for
// tests, and that is worth the twenty lines it costs.
//
// /dev/full is the youngest of the three and the only Linux invention among
// them. It is kept because os64 has never had ANY way to make a write fail on
// demand, and "tripwires over silence" cuts both ways: a house that tests its
// failure paths needs a surface that fails on purpose.
//
// ── THE ALIAS: why /dev/tty is not a file ───────────────────────────────────
//
// The other three are byte containers and answer through fops. /dev/tty is
// not — it is a NAME for a handle kind, and it resolves to exactly what
// os64_tty_handle() mints: a HANDLE_CONSOLE_IN (or _OUT) carrying no object,
// late-bound to task_tty(caller) at every single read. That late binding is
// what makes one tag serve a VT and a pty slave alike (see PTY.md, "The
// /dev/tty door"), and it is a property of the HANDLE, not of any file.
//
// It could not be done through fops even if the shape were tempting:
// HANDLE_FILE reads dispatch through call_in_kernel_context, which runs on
// the core's interrupt stack under kKernelPML4 — and console_read BLOCKS.
// Sleeping on that scratch stack is a known way to corrupt this kernel (the
// stack-poisoner tripwire caught the shutdown descent doing exactly that on
// 2026-08-13). So devfs answers the question at OPEN time instead, and
// syscall_open mints the console handle directly, which costs nothing and
// reuses the entire existing console path — Ctrl+C interception, EOF, tty
// focus — with not one line of special case downstream.
//
// This is what the breadcrumbs in syscall.c and io.h were waiting for: the
// verb came first because a pager needed it before a devfs existed, and the
// name NAMES the verb rather than rivalling it. Both spellings reach the same
// handle; one of them can be written in husk.rc.

#include <stdbool.h>

#include "driver/filesystem/vfs/vfs.h"
#include "handle.h"

// Claim "/dev" in the mount table. Called from kernel_init AFTER the root and
// the secondary-partition sweep, for the same two reasons procfs and sysfs
// are: devfs has no partition GUID at all (letting the sweep finish first
// keeps its all-zero GUID out of the dedupe comparisons), and it needs no
// filesystem underneath it — this mount works on a machine with no disk.
void devfs_mount(void);

// THE ALIAS HOOK (syscall_open's one question for devfs).
//
// "Does this fs-local path name a HANDLE KIND rather than a byte container?"
// True for exactly one path today — "/tty" — and *type receives the handle
// tag the requested mode calls for: reading gets HANDLE_CONSOLE_IN, writing
// or appending gets HANDLE_CONSOLE_OUT. The object is always NULL; a console
// handle carries none, by design.
//
// Deliberately a direct call rather than a new fops slot: there is ONE
// customer, and consumer-driven growth says size the seam to the demand. The
// day a second filesystem wants to alias a handle, this becomes the op-table
// entry it should have been, and this function becomes its implementation.
// Returns false for every other path (and for any fs that is not devfs), so
// the caller falls through to the ordinary open.
bool devfs_handle_alias(vfs_filesystem_t *fs, const char *path,
                        const char *mode, handle_type_t *type);

#endif
