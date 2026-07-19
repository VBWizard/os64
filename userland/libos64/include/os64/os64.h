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
#include "os64/io.h"
#include "os64/proc.h"
#include "os64/fmt.h"    // os64_printf/snprintf — columns without counting bytes
#include "os64/args.h"   // the arg parser (see its header for the anti-getopt case)
#include "os64/mem.h"    // os64_map/os64_unmap — the wall malloc builds on

#endif // OS64_H
