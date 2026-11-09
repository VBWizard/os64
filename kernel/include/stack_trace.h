#ifndef STACK_TRACE_H
#define STACK_TRACE_H

#include <stdint.h>
#include "task.h"

// Symbolized call-chain reporting for a faulting RING-3 task.
//
// Lineage: this is os32's stack_trace.c (2024, Chris) rebuilt for long mode.
// The design is his and the good parts are kept deliberately — a cascade of
// symbol sources, bounds re-checked on EVERY frame rather than only the first,
// a mapped-page test before every dereference, and two independent give-up
// counts. What changed is only what the hardware forced: an RBP chain instead
// of EBP, and the HHDM instead of a zero-based DS to read another address
// space's stack.
//
// SCOPE, v1: ring-3 faults only, named from the faulting program's own
// .symtab. Every os64 program is statically linked, so that one table covers
// the app AND all of libos64 — which is the whole program. Kernel-side
// symbolization (panics) needs the kernel's own symbols loaded at boot via
// Limine's kernel-file request and is a separate slice; so is DWARF file:line.
//
// Emits to the wire and the log always — a crash report that exists in only
// one place is the failure mode this whole slice was built to end — and to
// the glass when `glass` says so. A ring-3 death passes false: the person at
// the keyboard gets the one headline line, the chain is for the log reader
// (simple_exceptions.c FAULT_PRINT carries the ruling).
void stack_trace_user(task_t *task, uint64_t rip, uint64_t rbp, bool glass);

#endif // STACK_TRACE_H
