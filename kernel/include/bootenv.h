#ifndef BOOTENV_H
#define BOOTENV_H

#include "task.h"

// bootenv.h — the environment every task is born with, as a config file.
//
// `bootenv.conf` is looked up through the config search path (conf.h) and
// MERGED: the last directory on the ladder is applied first and the first
// last, so `/home/bootenv.conf` layers over `/etc/bootenv.conf` one variable
// at a time — your HOSTNAME and TZ sit over the system's, and the system's
// PATH stands unless you say otherwise. One `NAME = value` per line, names
// verbatim (they are data, not settings — the case rule in CLAUDE.md § The
// config search path), and a NAME with nothing after the `=` UNSETS it, the
// one power a merged file otherwise lacks.
//
// The kernel's built-in seed (PATH, HOSTNAME — kernel.c's create_kernel_task)
// is the floor beneath the file, so a boot with no bootenv.conf anywhere —
// the lifeboat's, or a broken root's — still has a path to walk. A `TZ=` on
// the boot cmdline outranks the file: it is typed for THIS boot, and it is
// the only channel that exists before a filesystem does. When present, that
// pair is seeded before file processing and every file-level TZ assignment
// or unset is ignored, so capacity cannot weaken the precedence rule.
//
// WHY A FILE. The first task's environment used to be C source — PATH,
// HOSTNAME, and TZ-if-the-cmdline-had-one — and `export TZ=...` in husk.rc
// was where a person's timezone lived. That worked while every program was a
// child of husk. The desktop is not: the kernel spawns it, it inherits the
// kernel's seed, and it never runs anybody's rc — so the clock it started
// from gui.conf showed UTC while the same clock started from husk showed
// Eastern. V7's /etc/profile (1979) was a script the login shell ran, which
// is husk.rc's ancestor and has the same blind spot; the data-file form is
// what things that are not shells need, and this is os64's.
//
// Applied ONCE, in kernel_init, to the kernel task's block after conf_init
// has settled the ladder and before the first program is spawned. The block
// starts at one page and grows on demand to TASK_ENV_MAX_BYTES while the
// files are merged; logd, the shells, the desktop and cron then inherit its
// completed allocation and contents.
void bootenv_apply(task_t *task);

#endif
