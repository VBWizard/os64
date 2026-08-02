// klog.c — reading the kernel log from ring 3 (the mechanism half lives in
// the kernel; see os64/klog.h for why the split falls here).

#include <stdint.h>
#include "os64/klog_read.h"
#include "os64/syscall.h"
#include "os64/syscall_numbers.h"

int64_t os64_klog_read(os64_logent_t *entries, uint32_t max_entries)
{
	return (int64_t)os64_syscall2(SYSCALL_KLOG_READ,
	                              (uint64_t)entries, (uint64_t)max_entries);
}
