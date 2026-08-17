#ifndef OS64_H
#define OS64_H

// libos64 — the os64 C library. This umbrella pulls in the pieces a simple
// program needs; larger programs can include the modular <os64/*.h> headers
// directly (see LIBOS64.md). libos64 links beside the `launch` startup stub.
//
// SCAFFOLDING PHASE: only the calls the first apps actually pull into
// existence are here (consumer-driven, per the roadmap — the API grows when
// an app demands it, exactly like syscalls do). Everything routes through
// the raw ABI stubs in <os64/syscall.h>; the friendly veneer lives here.

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>  // bool/true/false — GCC ships this even freestanding
                      // (no library behind it, same club as stdint.h); after
                      // 30 years nobody should have to remember what 0 means
#include "os64/io.h"
#include "os64/proc.h"
#include "os64/procfs.h" // typed readers for the text /proc task reports
#include "os64/fmt.h"    // os64_printf/snprintf — columns without counting bytes
#include "os64/args.h"   // the arg parser (see its header for the anti-getopt case)
#include "os64/mem.h"    // os64_map/os64_unmap — the wall malloc builds on
#include "os64/str.h"    // strlen/strcopy/streq — and the case against strcpy
#include "os64/date.h"   // os64_time/os64_date_now — the wall clock and calendar
#include "os64/klog_read.h"  // os64_klog_read — the kernel log, for the log daemon
#include "os64/thread.h"     // os64_thread — a second line of execution
#include "os64/dial.h"       // os64_dial("udp!10.0.2.2!53") — the network in one call
#include "os64/gui.h"        // syscalls 16-21 + the boundary structs — a window in one call
#include "os64/draw.h"       // libdraw — nobody sets a pixel by hand (LIBDRAW.md)

#endif // OS64_H
