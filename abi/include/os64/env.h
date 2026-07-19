#ifndef OS64_ABI_ENV_H
#define OS64_ABI_ENV_H

// The os64 environment block — kernel↔userland ABI, like syscall_numbers.h.
//
// Every task's environment lives in one (or more, someday) read-only pages
// the kernel maps at TASK_ENV_VIRT and hands to main() as its third argument
// (launch.S preserves the kernel-set RDI/RSI/RDX = argc/argv/env registers).
// Both sides walk the SAME layout, so the layout lives here, outside both.
//
// This is deliberately NOT os32's array of evenly-lengthed strings, and NOT
// POSIX's char **environ of "KEY=VALUE" strings that every getenv() has to
// re-split on '=' forever. It is packed key\0value\0 pairs with a small
// header — keys and values are each real NUL-terminated strings, no '='
// convention, no fixed widths, no pointer array to maintain:
//
//   [ uint32 page_count | uint32 count | uint32 data_end | data... ]
//   data: key0\0val0\0key1\0val1\0...
//
// data_end is the offset of the first free byte in data[]; count is the
// number of pairs. Userland treats the block as READ-ONLY (it is mapped
// read-only; environment changes happen at spawn time through the kernel).

#include <stdint.h>

typedef struct {
	uint32_t page_count;   // pages backing this block (>= 1)
	uint32_t count;        // number of key/value pairs
	uint32_t data_end;     // offset into data[] of the next free byte
	char     data[];       // packed key\0value\0 pairs
} os64_env_block_t;

#endif
