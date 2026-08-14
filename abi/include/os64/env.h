#ifndef OS64_ABI_ENV_H
#define OS64_ABI_ENV_H

// The os64 environment block — kernel↔userland ABI, like syscall_numbers.h.
//
// Every task's environment lives in one or more read-only pages the kernel
// maps at TASK_ENV_VIRT and hands to main() as its third argument (launch.S
// preserves the kernel-set RDI/RSI/RDX = argc/argv/env registers). Both
// sides walk the SAME layout, so the layout lives here, outside both.
//
// The block is born one page and GROWS when setenv fills it (2026-08-14):
// the kernel swaps a doubled block under the same TASK_ENV_VIRT window, up
// to a 64KB ceiling — after which setenv fails for real. Userland never
// notices a growth beyond page_count changing: the address is stable, and a
// setenv already invalidated prior getenv/env_next results (proc.h).
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

// The longest key or value os64_env_next will hand back. 255 like
// OS64_DIRENT_NAME_MAX, and named the same way — for the thing it bounds,
// NOT as a universal "max string". Different things have different natural
// limits (a path is TASK_MAX_PATH_LEN), and one global maximum would end up
// wrong for something and get quietly widened for everything.
//
// The "+ 1" below is the NUL, exactly as os64_dirent_t declares its name —
// so 255 means 255 USABLE characters in both structs rather than 255 in one
// and 254 in the other.
#define OS64_ENV_STR_MAX 255

// One environment entry, filled by os64_env_next. The key and value are
// COPIES, not pointers into the environment block: the block is shared,
// process-lifetime state that every later os64_getenv reads, and handing a
// caller a writable pointer into it means one stray strcpy corrupts the
// environment for the rest of the program's life. Two 256-byte buffers on the
// caller's stack is a cheap price for that not being possible.
//
// `index` is the walk's cursor and the caller's only bookkeeping: zero it to
// start, then hand the SAME struct back to keep going. State lives in the
// caller's struct rather than a library global for the reason getopt's optind
// is a fossil — a global cursor makes two loops over the environment
// (or one nested inside another) silently eat each other.
typedef struct {
    char key[OS64_ENV_STR_MAX + 1];
    char value[OS64_ENV_STR_MAX + 1];
    uint32_t index; //The index of the key in the environment block ... set to 0 to get the first
} os64_envent_t;

#endif
