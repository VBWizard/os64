#ifndef OS64_KLOG_READ_H
#define OS64_KLOG_READ_H

// os64/klog_read.h — libos64's door to the kernel log.
//
// The struct and the doctrine live in <os64/klog.h> (the ABI); this header
// is just the call. The daemon's whole loop:
//
//     os64_logent_t batch[64];
//     for (;;) {
//         int64_t n = os64_klog_read(batch, 64);
//         for (int64_t i = 0; i < n; i++) ...write batch[i]...
//         if (n == 0) os64_sleep(100);
//     }
//
// Remember that reading CLAIMS the log — while this is being called the
// kernel stops draining to serial, and if the caller dies or hangs the
// kernel takes it back within seconds. A program that reads the log and
// then throws the entries away is a program that silently deletes the
// system's logging; that is the caller's responsibility, by design.

#include <stdint.h>
#include "os64/klog.h"   // os64_logent_t — the abi contract

// Returns the number of entries taken (0 = none waiting; negative = refused).
// Entries are REMOVED from the kernel's rings: reading is consuming.
int64_t os64_klog_read(os64_logent_t *entries, uint32_t max_entries);

#endif // OS64_KLOG_READ_H
