#ifndef OS64_ACCOUNTING_H
#define OS64_ACCOUNTING_H

#include <stdbool.h>
#include <stdint.h>

#include "smp.h"

typedef struct {
	uint64_t runtime, user, system, idle, fault;
} accounting_thread_cycles_t;

typedef struct {
	uint64_t total, user, system, idle, sched;
} accounting_core_cycles_t;

// cs is the interrupted CS, or zero for a known kernel-side boundary.
// The syscall and scheduler return stubs call this after preserving registers;
// exception entry and return use the functions below to bracket #PF work.
void accounting_charge(uint64_t cs);
void accounting_exception_enter(uint64_t interrupted_cs, uint64_t vector);
void accounting_exception_exit(uint64_t vector);
void accounting_begin_scheduler(uint64_t interrupted_cs);
void accounting_end_scheduler(void);
void accounting_halt_begin(void);
void accounting_halt_end(void);
bool accounting_read_thread(const thread_t *thread, accounting_thread_cycles_t *out);
bool accounting_read_core(const core_local_storage_t *cls, accounting_core_cycles_t *out);

#endif
