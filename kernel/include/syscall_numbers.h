#ifndef SYSCALL_NUMBERS_H
#define SYSCALL_NUMBERS_H

// The syscall numbers live in abi/include/os64/ — THE kernel↔userland
// contract, deliberately outside kernel source (see that header's comment).
// This shim exists so kernel code and assembly can keep saying
// "syscall_numbers.h"; it must never define numbers of its own. (The kernel
// once kept a full copy here. It drifted from the abi copy within a week —
// SYSCALL_GUI_EVENT_WAIT existed on one side only — which is exactly the bug
// class abi/ was created to end. A shim can't drift.)
//
// Preprocessor-only requirement still holds: this is included from assembly
// (task_exit_asm.S, syscall.S), and the abi header honors it.

#include "os64/syscall_numbers.h"

#endif
