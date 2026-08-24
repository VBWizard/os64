#include "syscall.h"
#include "syscall_numbers.h"

#include <stddef.h>
#include <stdint.h>

#include "BasicRenderer.h"
#include "panic.h"
#include "printd.h"
#include "serial_logging.h"   // serial_print_string — debug_log's SERIAL beacon flag
#include "strings/sprintf.h"  // snprintf — stitches the beacon line for one wire write
#include "scheduler.h"
#include "smp_core.h"
#include "task.h"
#include "memory/memcpy.h"
#include "memory/paging.h"
#include "memory/kmalloc.h"
#include "memory/vma.h"   // call_in_kernel_context
#include "log.h"
#include "os64/klog.h"     // os64_logent_t — klog_read's out-struct (abi)
#include "thread_join.h"   // the object behind HANDLE_THREAD
#include "console.h"
#include "gui/gui_client.h"   // the GUI client API rows 16-21 dispatch into
#include "gui/window.h"       // GUI_WINDOW_TITLE_MAX — the title copy's bound
#include "tty.h"           // console writes land on the CALLER's terminal now
#include "conf.h"          // conf_find — the config search path, walked in one place
#include "handle.h"
#include "driver/filesystem/dev/devfs.h"   // devfs_handle_alias — the /dev/tty door
#include "pipe.h"
#include "signals.h"
#include "gdt.h"     // GDT_USER_* — sigreturn's full path rebuilds ring-3 selectors from constants
#include "os64/signal.h"   // OS64_SIG_ERR_* — the errors ring 3 is told
#include "vfs.h"     // kRootFilesystem + vfs_file_t (open/seek/file read/write)
#include "shutdown.h"   // shutdown_system — SYSCALL_SHUTDOWN's engine
#include "allocator.h"  // free_memory — unmap returns pages at the choke point
#include "dlist.h"      // dlist_remove (unmap drops the region's VMA node)
#include "memory/mmap.h"   // MAP_ANONYMOUS (map()'s regions are anonymous)
#include "CONFIG.h"        // TICKS_PER_SECOND — sleep()'s ms→ticks boundary
#include "os64/ticks.h"    // os64_ticks_t — the ticks() out-struct (abi)
#include "os64/memory.h"   // os64_memory_t — the memory() out-struct (abi)
#include "os64/time.h"     // os64_time_t — the time() out-struct (abi)
#include "os64/pty.h"      // os64_pty_header_t/_cell_t — pty_snapshot's out-structs (abi)
#include "env.h"           // env_set/env_unset — setenv() mutates the task's env block
#include "os64/net.h"      // os64_netdest_t — net_dial's in-struct (abi)
#include "driver/net/net_device.h"   // kNetDevices — dial needs a NIC to dial on
#include "driver/net/net_wire.h"     // NET_IPV4_OCTETS — address logging
#include "driver/net/udp_conn.h"     // the object behind HANDLE_NET_UDP
#include "driver/net/tcp.h"          // ...and HANDLE_NET_TCP
#include "driver/net/icmp_conn.h"    // ...and HANDLE_NET_ICMP

// The monotonic tick counter (kernel.h) — read by sleep()'s deadline math
// and handed to ring 3 by ticks().
extern volatile uint64_t kTicksSinceStart;
extern volatile uint64_t kSystemCurrentTime;   // UTC epoch seconds (timer IRQ advances it)
extern volatile uint64_t irq0_current_count;   // ticks into the current second (same IRQ)
extern uint64_t kTicksPerSecond;
extern int kTimeZone;                          // configured zone, HOURS east of UTC
extern uint64_t kTotalMemory;      // installed RAM (memmap.c, Limine map sum)
extern uint64_t kAvailableMemory;  // USABLE entries only — what the allocator governs

// spawn: cap on argv length. A command line's worth of args is plenty; the
// per-arg length cap is task.c's TASK_MAX_PATH_LEN (the blob it builds uses
// dynamically sized slots up to width).
#define SPAWN_MAX_ARGS 512

#define SYSCALL_RESULT_INVALID UINT64_C(0xFFFFFFFFFFFFFFFF)
#define SYSCALL_RESULT_BAD_USER_DATA UINT64_C(0xFFFFFFFFFFFFFFFE)

static uint64_t g_saved_cr3[MAX_CPUS];
static bool g_saved_cr3_valid[MAX_CPUS];

static inline uint32_t get_current_cpu_index(void);
static bool prepare_syscall_args(const syscall_entry_t *entry, const uint64_t incoming[6], uint64_t prepared[6]);
static void raise_terminating_signal_and_die(task_t *task, thread_t *thread);   // defined with its SIGPIPE twin below
static bool copy_user_string(const char *user_str, char *buffer, size_t buffer_len);
static bool copy_user_buffer(const void *user_src, void *kernel_dst, size_t length);
static bool copy_to_user_buffer(void *user_dst, const void *kernel_src, size_t length);

static uint64_t syscall_yield(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_read(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_spawn(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_wait(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_debug_log(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_exit(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_write(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_pipe(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_pty_create(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_pty_snapshot(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_close(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_open(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_seek(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_readdir(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_map(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_unmap(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_getcwd(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_chdir(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_unlink(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_mkdir(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_rename(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_stat(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_reap(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_sleep(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_ticks(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_memory(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_printat(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_time(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_set_time(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_setenv(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_klog_read(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_sync(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_thread(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_thread_exit(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_net_dial(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_sync_all(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_shutdown(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_getpid(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_heap_report(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_tty_handle(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_conf_resolve(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_gui_window_create(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_gui_window_destroy(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_gui_window_get_surface(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_gui_window_get_state(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_signal_handler(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_sigreturn(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_gui_window_publish(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_gui_event_poll(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_gui_screen_info(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_gui_event_wait(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);

// NOTE: syscall.S marshals the syscall registers straight into
// _syscall_dispatch()'s C arguments — there is deliberately no C-level entry
// shim here (the old register-pinned-locals version relied on behavior GCC
// only guarantees for inline-asm operands).
// Last column is user_ptr_arg_mask: bit i = "arg i is a user pointer, range-
// check it at the boundary".  Non-pointer args (handles, lengths, exit codes)
// and unused arg registers (ring-3 garbage!) must NOT be checked.
//
// needs_cr3_switch is FALSE for every current entry, on purpose.  Handlers run
// on the calling task's CR3, which maps BOTH the user's buffers (lower half)
// AND the whole kernel (upper half is shared into every task PML4) — there is
// nothing a syscall needs that the user CR3 can't see.  Switching to
// kKernelPML4 mid-syscall is actively fatal: the thread's syscall kernel stack
// is a task-local VA that kKernelPML4 does NOT map, so the first C statement
// after the switch touches an unmapped stack -> #PF -> the #PF handler pushes
// onto the same unmapped stack -> double -> TRIPLE FAULT (write() proved this
// the very first time the flag was ever exercised).  If a future syscall
// genuinely needs kernel context, it must switch STACK and CR3 together — see
// call_in_kernel_context in task_exit_asm.S for the proven pattern.
syscall_entry_t syscall_table[MAX_SYSCALLS] = {
	SYSCALL_DEFINE(SYSCALL_YIELD,     "yield",     syscall_yield,     false, 0x00),
	SYSCALL_DEFINE(SYSCALL_DEBUG_LOG, "debug_log", syscall_debug_log, false, 0x01),  // arg0 = message
	SYSCALL_DEFINE(SYSCALL_EXIT,      "exit",      syscall_exit,      false, 0x00),
	SYSCALL_DEFINE(SYSCALL_WRITE,     "write",     syscall_write,     false, 0x02),  // arg1 = buffer
	SYSCALL_DEFINE(SYSCALL_READ,      "read",      syscall_read,      false, 0x02),  // arg1 = buffer (written)
	SYSCALL_DEFINE(SYSCALL_SPAWN,     "spawn",     syscall_spawn,     false, 0x03),  // arg0 = path, arg1 = argv (args 2-4 = in/out/err handles, NOT pointers)
	SYSCALL_DEFINE(SYSCALL_WAIT,      "wait",      syscall_wait,      false, 0x02),  // arg1 = exit-code out ptr
	SYSCALL_DEFINE(SYSCALL_PIPE,      "pipe",      syscall_pipe,      false, 0x01),  // arg0 = int[2] out
	SYSCALL_DEFINE(SYSCALL_CLOSE,     "close",     syscall_close,     false, 0x00),  // arg0 = handle (an int, not a pointer)
	SYSCALL_DEFINE(SYSCALL_OPEN,      "open",      syscall_open,      false, 0x03),  // arg0 = path, arg1 = mode (both strings)
	SYSCALL_DEFINE(SYSCALL_SEEK,      "seek",      syscall_seek,      false, 0x00),  // args: handle, offset, whence — no pointers
	SYSCALL_DEFINE(SYSCALL_READDIR,   "readdir",   syscall_readdir,   false, 0x02),  // arg1 = os64_dirent_t out ptr
	SYSCALL_DEFINE(SYSCALL_MAP,       "map",       syscall_map,       false, 0x00),  // arg0 = length (not a pointer)
	SYSCALL_DEFINE(SYSCALL_UNMAP,     "unmap",     syscall_unmap,     false, 0x01),  // arg0 = region base (user VA)
	SYSCALL_DEFINE(SYSCALL_GETCWD,    "getcwd",    syscall_getcwd,    false, 0x01),  // arg0 = out buffer
	SYSCALL_DEFINE(SYSCALL_CHDIR,     "chdir",     syscall_chdir,     false, 0x01),  // arg0 = path
	SYSCALL_DEFINE(SYSCALL_UNLINK,    "unlink",    syscall_unlink,    false, 0x01),  // arg0 = path
	SYSCALL_DEFINE(SYSCALL_STAT,      "stat",      syscall_stat,      false, 0x03),  // arg0 = path, arg1 = dirent out ptr
	SYSCALL_DEFINE(SYSCALL_REAP,      "reap",      syscall_reap,      false, 0x01),  // arg0 = exit-code out ptr
	SYSCALL_DEFINE(SYSCALL_SLEEP,     "sleep",     syscall_sleep,     false, 0x00),  // arg0 = milliseconds (a value, no pointers)
	SYSCALL_DEFINE(SYSCALL_TICKS,     "ticks",     syscall_ticks,     false, 0x01),  // arg0 = os64_ticks_t out ptr
	SYSCALL_DEFINE(SYSCALL_MEMORY,    "memory",    syscall_memory,    false, 0x01),  // arg0 = os64_memory_t out ptr
	SYSCALL_DEFINE(SYSCALL_PRINTAT,   "printat",   syscall_printat,   false, 0x04),  // arg0 = x cell, arg1 = y cell, arg2 = string
	SYSCALL_DEFINE(SYSCALL_TIME,      "time",      syscall_time,      false, 0x01),  // arg0 = os64_time_t out ptr
	SYSCALL_DEFINE(SYSCALL_SETENV,    "setenv",    syscall_setenv,    false, 0x01),  // arg0 = key; arg1 = value OR NULL (mask excludes it: NULL means unset)
	SYSCALL_DEFINE(SYSCALL_KLOG_READ, "klog_read", syscall_klog_read, false, 0x01),  // arg0 = os64_logent_t[] out, arg1 = max entries
	SYSCALL_DEFINE(SYSCALL_SYNC,      "sync",      syscall_sync,      false, 0x00),  // arg0 = handle (an int, not a pointer)
	// arg0/arg2 are CODE addresses in the caller's own text, not buffers —
	// they are not in the pointer mask because user_range_accessible checks
	// data reachability, and thread_join_create validates the stack mapping
	// it actually writes to.
	SYSCALL_DEFINE(SYSCALL_THREAD,      "thread",      syscall_thread,      false, 0x00),
	SYSCALL_DEFINE(SYSCALL_THREAD_EXIT, "thread_exit", syscall_thread_exit, false, 0x00),
	SYSCALL_DEFINE(SYSCALL_MKDIR,       "mkdir",       syscall_mkdir,       false, 0x01),  // arg0 = path
	SYSCALL_DEFINE(SYSCALL_RENAME,      "rename",      syscall_rename,      false, 0x03),  // arg0 = old path, arg1 = new path
	SYSCALL_DEFINE(SYSCALL_PTY_CREATE,   "pty_create",   syscall_pty_create,   false, 0x00),  // args: cols, rows — values, no pointers
	SYSCALL_DEFINE(SYSCALL_PTY_SNAPSHOT, "pty_snapshot", syscall_pty_snapshot, false, 0x02),  // arg1 = header out; arg2 = cells out, NULLABLE (max_cells 0 = header-only probe — handler validates)
	SYSCALL_DEFINE(SYSCALL_NET_DIAL,  "net_dial",  syscall_net_dial,  false, 0x01),  // arg0 = os64_netdest_t in ptr
	SYSCALL_DEFINE(SYSCALL_SYNC_ALL,  "sync_all",  syscall_sync_all,  false, 0x00),  // no args — the broom sweeps the whole floor
	SYSCALL_DEFINE(SYSCALL_SHUTDOWN,  "shutdown",  syscall_shutdown,  false, 0x00),  // no args, no return — the ordered descent (shutdown.c)
	SYSCALL_DEFINE(SYSCALL_GETPID,    "getpid",    syscall_getpid,    false, 0x00),  // no args — who am I? (V1's question, V1's answer: a number in a register)
	SYSCALL_DEFINE(SYSCALL_SET_TIME,  "set_time",  syscall_set_time,  false, 0x00),  // arg0 = UTC epoch bits; monotonic clock is untouched
	SYSCALL_DEFINE(SYSCALL_HEAP_REPORT, "heap_report", syscall_heap_report, false, 0x01),  // arg0 = user VA of an os64_heap_report_t (0 withdraws)
	SYSCALL_DEFINE(SYSCALL_TTY_HANDLE, "tty_handle", syscall_tty_handle, false, 0x00),  // no args — the answer is a property of the ASKER
	SYSCALL_DEFINE(SYSCALL_CONF_RESOLVE, "conf_resolve", syscall_conf_resolve, false, 0x03),  // arg0 = name in, arg1 = path out; arg4 = don't probe
	// ── GUI (16-21): GRAPHICS.md's userland boundary, live 2026-08-17.
	// Ownership (migration step 1) went in BEFORE these doors opened: every
	// handle below is checked against its owner inside gui_client.c, so a
	// task can never touch a window it did not create. 22 (event_wait) ships
	// LAST with the block/wake plumbing (migration step 5). Nullable
	// pointers stay OUT of the masks per the SETENV precedent — their
	// handlers validate the copy instead.
	SYSCALL_DEFINE(SYSCALL_GUI_WINDOW_CREATE,      "gui_window_create",      syscall_gui_window_create,      false, 0x01),  // arg0 = title string
	SYSCALL_DEFINE(SYSCALL_GUI_WINDOW_DESTROY,     "gui_window_destroy",     syscall_gui_window_destroy,     false, 0x00),  // arg0 = handle (a number, not a pointer)
	SYSCALL_DEFINE(SYSCALL_GUI_WINDOW_GET_SURFACE, "gui_window_get_surface", syscall_gui_window_get_surface, false, 0x02),  // arg1 = surface_t out
	SYSCALL_DEFINE(SYSCALL_GUI_WINDOW_PUBLISH,     "gui_window_publish",     syscall_gui_window_publish,     false, 0x00),  // arg1 = rect_t in OR NULL (nullable — handler validates)
	SYSCALL_DEFINE(SYSCALL_GUI_EVENT_POLL,         "gui_event_poll",         syscall_gui_event_poll,         false, 0x02),  // arg1 = input_event_t out
	SYSCALL_DEFINE(SYSCALL_GUI_SCREEN_INFO,        "gui_screen_info",        syscall_gui_screen_info,        false, 0x00),  // arg0/arg1 = uint32_t outs, EITHER may be NULL (handler validates)
	SYSCALL_DEFINE(SYSCALL_GUI_EVENT_WAIT,         "gui_event_wait",         syscall_gui_event_wait,         false, 0x02),  // arg1 = input_event_t out; BLOCKS (like read)
	SYSCALL_DEFINE(SYSCALL_GUI_WINDOW_GET_STATE,   "gui_window_get_state",   syscall_gui_window_get_state,   false, 0x02),  // arg1 = os64_gui_window_state_t out
	// arg1 is a CODE address the kernel will one day jump to, not a buffer it
	// reads — so it stays OUT of the pointer mask (which validates readable
	// user memory) and the handler range-checks it itself.
	SYSCALL_DEFINE(SYSCALL_SIGNAL_HANDLER,         "signal_handler",         syscall_signal_handler,         false, 0x00),  // arg0 = signo, arg1 = handler
	// arg0 is the frame, VALIDATED by the handler itself (magic + the running
	// handler check) rather than by the pointer mask — the mask proves a buffer
	// is readable, which is the least of what this one has to prove.
	SYSCALL_DEFINE(SYSCALL_SIGRETURN,              "sigreturn",              syscall_sigreturn,              false, 0x00),  // arg0 = the signal frame
};

uint64_t _syscall_dispatch(
	uint64_t syscall_number,
	uint64_t arg0, uint64_t arg1, uint64_t arg2,
	uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	const uint64_t raw_args[6] = { arg0, arg1, arg2, arg3, arg4, arg5 };
	uint64_t prepared_args[6] = {0};
	bool switched_cr3 = false;
	uint64_t entry_cr3 = 0;

	// The terminate checkpoint. Every task that is DOING anything passes
	// through here constantly (cat's write loop = thousands of crossings a
	// second), so a pending terminate — set at a keystroke (Ctrl+C) or by a
	// write to /proc/<id>/ctl — is enforced within microseconds, and in the
	// victim's own context, where task_exit is safe. Tasks that are BLOCKED
	// instead get here via their woken blocking loops (see
	// raise_terminating_signal_and_die). Only a syscall-free ring-3 spin
	// evades this boundary entirely — and the forced-syscall push in
	// scheduler.c closes that gap.
	// Hoisted out of the block below so the DELIVERY hook at the end of this
	// function can reuse them — one lookup, and the two halves of the signal
	// story (default action on the way in, handler on the way out) provably
	// talk about the same thread.
	core_local_storage_t *sig_cls = get_core_local_storage();
	thread_t *sig_thread = sig_cls ? sig_cls->currentThread : NULL;
	// THE TASK COMES FROM THE THREAD, NEVER FROM CLS — and this is not
	// pedantry, it is a bug that shipped and was caught on glass the same day
	// (2026-08-23). cls->task and cls->currentThread are not updated
	// atomically, so for a window right after a thread MIGRATES CORES they
	// disagree: the core has the new thread but still names the task it was
	// idling for. Reading the handler table out of cls->task then asks the
	// IDLE task whether it installed a handler, gets "no", and kills a
	// program that had one — intermittently, depending on whether the thread
	// happened to move. thread->ownerTask cannot be out of sync with the
	// thread, because it IS part of the thread.
	//
	// (The old code passed cls->task here too, but only ever on the way to
	// dying anyway. Making a DECISION depend on it is what turned a cosmetic
	// staleness into a lost program.)
	task_t *sig_task = sig_thread ? (task_t *)sig_thread->ownerTask : NULL;
	{
		// A pending terminate kills only when NOTHING will catch it. A task
		// that installed a handler for this signal gets it delivered on the
		// way out instead — which is the whole point of the registration
		// syscall, and why the default action has to ask first.
		if (sig_thread && (sigset_any(sig_thread->signals.sigind, SIGNALS_TERMINATING)) &&
		    !signal_has_handler_for_pending(sig_task, sig_thread))
			raise_terminating_signal_and_die(sig_task, sig_thread);
	}

	// Remember the address space we arrived on for the exit tripwire below.
	asm volatile("mov %0, cr3" : "=r"(entry_cr3));

	if (syscall_number >= MAX_SYSCALLS)
	{
        printd(DEBUG_SYSCALL, "SYSCALL: invalid number %lu\n", syscall_number);
        return SYSCALL_RESULT_INVALID;
	}

	syscall_entry_t *entry = &syscall_table[syscall_number];
	if (!entry->func)
	{
        printd(DEBUG_SYSCALL, "SYSCALL: unimplemented number %lu\n", syscall_number);
        return SYSCALL_RESULT_INVALID;
	}

	if (!prepare_syscall_args(entry, raw_args, prepared_args))
	{
        printd(DEBUG_SYSCALL, "SYSCALL: user argument validation failed for %lu\n", syscall_number);
        return SYSCALL_RESULT_BAD_USER_DATA;
	}

	if (entry->needs_cr3_switch)
	{
		switch_to_kernel_cr3();
		switched_cr3 = true;
	}

	if (entry->trace_enabled)
	{
		log_syscall_invocation(entry, prepared_args);
	}

	uint64_t result = entry->func(
		prepared_args[0], prepared_args[1], prepared_args[2],
		prepared_args[3], prepared_args[4], prepared_args[5]);

	if (switched_cr3)
	{
		restore_user_cr3();
	}

	// Boundary tripwire (a keeper from the 32-bit OS): a syscall must leave on
	// the same address space it arrived on.  Escaping to ring 3 with the kernel
	// CR3 still loaded "works" (kernel maps are a superset) right up until it
	// corrupts something unrelated, so catch it here where the cause is obvious.
	// (Threads that legitimately ARRIVED on the kernel CR3 are exempt.)
	uint64_t exit_cr3 = 0;
	asm volatile("mov %0, cr3" : "=r"(exit_cr3));
	if (exit_cr3 != entry_cr3)
	{
		panic("_syscall_dispatch: syscall %lu (%s) entered on CR3 %#lx but is leaving on %#lx\n",
		      syscall_number, entry->name ? entry->name : "(unnamed)", entry_cr3, exit_cr3);
	}

	// ── SIGNAL DELIVERY, on the way out ─────────────────────────────────────
	//
	// HERE, and not at the checkpoint on the way IN, because the syscall's
	// return value has to survive the handler. Delivering before the body ran
	// would skip the syscall entirely and resume afterwards as though it had
	// happened — a `read` that silently never read. Delivering here means the
	// call completes, its answer goes into the frame, and sigreturn puts it
	// back in RAX.
	//
	// One place, not nine: the nine checkpoints exist so a PARKED thread
	// notices a terminate in its own context, and they still do. Every one of
	// them returns through here, so this is where a handler gets armed.
	//
	// Deliberately last: after the CR3 tripwire, so a delivery can never be
	// blamed for an address-space escape it did not cause, and after the
	// syscall body, so `result` is the real answer.
	// Same rule as the entry checkpoint: the task comes from the THREAD.
	// Delivering against cls->task would build a frame from one program's
	// handler table onto another program's stack.
	if (sig_thread != NULL && sig_task != NULL &&
	    signal_deliver_pending(sig_task, sig_thread, result) == SIGNAL_DELIVER_FAILED)
	{
		// A handled signal is pending and the frame could not be written —
		// the stack is unusable (SIGNALS.md §9). The handler cannot run, so
		// the DEFAULT ACTION does, here in the victim's own context, exactly
		// as if no handler had been installed. The alternative — shrugging —
		// leaves a signal that neither delivers nor kills, and a program
		// livelocked on INTERRUPTED (see signal_deliver_result_t).
		raise_terminating_signal_and_die(sig_task, sig_thread);
		__builtin_unreachable();
	}

	return result;
}

void switch_to_kernel_cr3(void)
{
	uint64_t current_cr3 = 0;
	asm volatile("mov %0, cr3" : "=r"(current_cr3));

	uint32_t cpu_index = get_current_cpu_index();
	g_saved_cr3[cpu_index] = current_cr3;
	g_saved_cr3_valid[cpu_index] = true;

	if (current_cr3 != (uint64_t)kKernelPML4)
	{
		asm volatile("mov cr3, %0" :: "r"((uint64_t)kKernelPML4) : "memory");
	}
}

void restore_user_cr3(void)
{
	uint32_t cpu_index = get_current_cpu_index();
	if (!g_saved_cr3_valid[cpu_index])
	{
		return;
	}

	uint64_t user_cr3 = g_saved_cr3[cpu_index];
	g_saved_cr3_valid[cpu_index] = false;

	if (user_cr3 && user_cr3 != (uint64_t)kKernelPML4)
	{
		asm volatile("mov cr3, %0" :: "r"(user_cr3) : "memory");
	}
}

// ── User-range pre-validation ────────────────────────────────────────────────
// The copy helpers below memcpy through user VAs in ring 0.  A fault during
// that copy is FINE when the page is demand-pageable — the #PF handler maps it
// and the access retries — but a WILD pointer (no VMA, nothing mapped) faults
// in KERNEL mode, and a kernel-mode fault with no VMA is, correctly, a panic.
// An app must not be able to panic the OS by handing a syscall a garbage
// pointer (ls did exactly that, 2026-07-22), so every page of a user range is
// vetted before the memcpy touches it:
//   - VMA-covered → legal: a not-present page is just demand paging waiting
//     to happen.  For copy-OUT the VMA must also be writable — a store to a
//     read-only page would be a ring-0 protection violation, another panic
//     door.  (CoW pages pass correctly: their VMA says PROT_WRITE and the
//     write fault resolves through the CoW branch.)
//   - No VMA but present in the task's tables → legal: covers kernel-created
//     eager mappings; the PTE's own W bit answers the writability question.
//   - Neither → wild pointer; reject, so the syscall fails with
//     SYSCALL_RESULT_BAD_USER_DATA instead of the kernel faulting.
static bool user_range_accessible(const void *user_ptr, size_t length, bool for_write)
{
	core_local_storage_t *cls = get_core_local_storage();
	task_t *task = cls ? cls->task : NULL;
	if (task == NULL)
		return false;

	uintptr_t addr = (uintptr_t)user_ptr;
	uintptr_t end = addr + length;   // no overflow: callers range-check vs kHHDMOffset first

	for (uintptr_t page = addr & ~(uintptr_t)(PAGE_SIZE - 1); page < end; page += PAGE_SIZE)
	{
		vma_t *vma = vma_lookup(task, page);
		if (vma != NULL)
		{
			if (for_write && !(vma->prot & PROT_WRITE))
				return false;
			continue;
		}

		uintptr_t pte = paging_walk_paging_table_keep_flags((pt_entry_t *)task->pml4v, page, true);
		if (pte == 0xbadbadba || !(pte & PAGE_PRESENT))
			return false;
		if (for_write && !(pte & PAGE_WRITE))
			return false;
	}

	return true;
}

bool validate_and_copy_user_data(const void* user_ptr, size_t length, void* kernel_buffer)
{
	if (!user_ptr || !kernel_buffer || length == 0)
	{
		return false;
	}

	uintptr_t user_address = (uintptr_t)user_ptr;

	// Reject ranges that start in — or run into — kernel-mapped memory.  The
	// subtraction form also catches user_address+length overflowing to wrap
	// back below kHHDMOffset.
	if (user_address >= kHHDMOffset || length > kHHDMOffset - user_address)
	{
		return false;
	}

	if (!user_range_accessible(user_ptr, length, false))
	{
		return false;
	}

	memcpy(kernel_buffer, user_ptr, length);
	return true;
}

void log_syscall_invocation(const syscall_entry_t* entry, const uint64_t args[6])
{
	if (!entry)
	{
		return;
	}

	size_t index = (size_t)(entry - syscall_table);
	const char *name = entry->name ? entry->name : "(unnamed)";
    printd(DEBUG_SYSCALL,
           "SYSCALL: #%zu %s args=%#lx,%#lx,%#lx,%#lx,%#lx,%#lx\n",
           index, name,
           args[0], args[1], args[2], args[3], args[4], args[5]);
}

static inline uint32_t get_current_cpu_index(void)
{
	core_local_storage_t *cls = get_core_local_storage();
	if (!cls)
	{
		return 0;
	}

	uint64_t apic_id = cls->apic_id;
	if (apic_id >= MAX_CPUS)
	{
		return 0;
	}

	return (uint32_t)apic_id;
}

// Boundary validation of user-pointer arguments, driven by the entry's
// user_ptr_arg_mask.  Only args the table declares as pointers are checked —
// see the mask's comment in syscall.h for why checking all six is a bug, not
// extra safety.  NULL is allowed through here so handlers can give it a
// per-call meaning (the copy helpers reject NULL where it matters).
static bool prepare_syscall_args(const syscall_entry_t *entry, const uint64_t incoming[6], uint64_t prepared[6])
{
	memcpy(prepared, incoming, sizeof(uint64_t) * 6);

	for (size_t i = 0; i < 6; ++i)
	{
		if (!(entry->user_ptr_arg_mask & (1u << i)))
		{
			continue;
		}

		if (prepared[i] == 0)
		{
			continue;
		}

		if (prepared[i] >= kHHDMOffset)
		{
			return false;
		}
	}

	return true;
}

// ── User-copy CR3 window ─────────────────────────────────────────────────────
// The dispatcher may already have moved this core to the kernel CR3 (entries
// with needs_cr3_switch), but user pointers only resolve under the USER CR3.
// The copy helpers below therefore run inside a "window": open() drops back to
// the user CR3 the dispatcher saved for this core (when there is one), and
// close() restores whatever was loaded before.  Interrupts are masked for the
// whole syscall (SFMASK clears IF), so the window can't be preempted while the
// "wrong" CR3 is live.
static uint64_t user_cr3_window_open(bool *switched)
{
        uint64_t original_cr3 = 0;
        asm volatile("mov %0, cr3" : "=r"(original_cr3));

        *switched = false;
        if (original_cr3 == (uint64_t)kKernelPML4)
        {
                uint32_t cpu_index = get_current_cpu_index();
                if (g_saved_cr3_valid[cpu_index])
                {
                        uint64_t user_cr3 = g_saved_cr3[cpu_index];
                        if (user_cr3 && user_cr3 != (uint64_t)kKernelPML4)
                        {
                                asm volatile("mov cr3, %0" :: "r"(user_cr3) : "memory");
                                *switched = true;
                        }
                }
        }

        return original_cr3;
}

static void user_cr3_window_close(uint64_t original_cr3, bool switched)
{
        if (switched)
        {
                asm volatile("mov cr3, %0" :: "r"(original_cr3) : "memory");
        }
}

// Copy a NUL-terminated string from user space into a kernel buffer, walking
// byte-by-byte so an unterminated user string can't run past buffer_len.
// Returns false if the user range is invalid or no NUL appears within the
// buffer (the buffer is still NUL-terminated for safe logging either way).
static bool copy_user_string(const char *user_str, char *buffer, size_t buffer_len)
{
        if (!user_str || !buffer || buffer_len == 0)
        {
                return false;
        }

        bool switched = false;
        uint64_t original_cr3 = user_cr3_window_open(&switched);

        bool success = false;
        size_t written = 0;
        while (written < buffer_len - 1)
        {
                char ch = 0;
                if (!validate_and_copy_user_data(user_str + written, sizeof(char), &ch))
                {
                        goto out;
                }

                buffer[written++] = ch;
                if (ch == '\0')
                {
                        success = true;
                        goto out;
                }
        }

        buffer[buffer_len - 1] = '\0';

out:
        user_cr3_window_close(original_cr3, switched);
        return success;
}

// Copy exactly `length` bytes from user space into a kernel buffer.  The
// length-based sibling of copy_user_string, for payloads that aren't strings
// (write() buffers may legally contain NUL bytes).
static bool copy_user_buffer(const void *user_src, void *kernel_dst, size_t length)
{
        if (!user_src || !kernel_dst || length == 0)
        {
                return false;
        }

        bool switched = false;
        uint64_t original_cr3 = user_cr3_window_open(&switched);

        bool success = validate_and_copy_user_data(user_src, length, kernel_dst);

        user_cr3_window_close(original_cr3, switched);
        return success;
}

// Copy `length` bytes from a kernel buffer OUT to user space — the reverse of
// copy_user_buffer, for read()'s result. Range-checks the destination the same
// way (reject any dst that reaches into kernel-mapped memory, overflow-safe).
static bool copy_to_user_buffer(void *user_dst, const void *kernel_src, size_t length)
{
        if (!user_dst || !kernel_src || length == 0)
        {
                return false;
        }

        uintptr_t user_address = (uintptr_t)user_dst;
        if (user_address >= kHHDMOffset || length > kHHDMOffset - user_address)
        {
                return false;
        }

        if (!user_range_accessible(user_dst, length, true))
        {
                return false;
        }

        bool switched = false;
        uint64_t original_cr3 = user_cr3_window_open(&switched);

        memcpy(user_dst, kernel_src, length);

        user_cr3_window_close(original_cr3, switched);
        return true;
}

static uint64_t syscall_yield(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg0;
	(void)arg1;
	(void)arg2;
	(void)arg3;
	(void)arg4;
	(void)arg5;

	// scheduler_trigger: genuine APIC self-IPI into the scheduler, same entry
	// semantics as the timer path (in-service bit, EOI discipline).  Returns
	// as soon as the scheduler decides — immediately if nothing else is
	// runnable, after a context-switch round trip if something was.
	// (Replaced scheduler_yield, whose direct software `int` bypassed APIC
	// nesting protection and whose empty-queue path slept a full tick.)
	scheduler_trigger(NULL);
	return 0;
}

static uint64_t syscall_debug_log(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg2;
	(void)arg3;
	(void)arg4;
	(void)arg5;

	const char *user_message = (const char*)arg0;
    char kernel_buffer[MAX_LOG_MESSAGE_SIZE];

    if (!copy_user_string(user_message, kernel_buffer, sizeof(kernel_buffer)))
	{
		return SYSCALL_RESULT_BAD_USER_DATA;
	}

	printd(DEBUG_APPLICATION,"[user] %s\n", kernel_buffer);

	// arg1 = flags (see abi syscall_numbers.h). SERIAL: put the line on the
	// wire NOW, directly — a beacon that no log claim can redirect. One
	// stitched buffer, one call, so concurrent beacons from other cores
	// interleave between lines rather than inside them. The ring copy above
	// still happens: a beacon is a log line too ("never drop a byte" cuts
	// both ways — the file must not be missing lines the wire showed).
	if (arg1 & OS64_DEBUG_LOG_SERIAL)
	{
		char wire[MAX_LOG_MESSAGE_SIZE + 16];
		snprintf(wire, sizeof(wire), "[user] %s\n", kernel_buffer);
		serial_print_string(wire);
	}
	return 0;
}

// exit(code) — terminate the calling task.  arg0 becomes the task's retVal
// (harvested by task_wait / the test harness).  Everything after the retVal
// store is task_exit()'s problem: it switches to the per-core kernel interrupt
// stack and kKernelPML4 itself, marks task+thread exited, and yields away for
// good — which is why this entry runs with needs_cr3_switch=false (the
// dispatcher's save/restore bookkeeping would never get to run anyway).
static uint64_t syscall_exit(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg1;
	(void)arg2;
	(void)arg3;
	(void)arg4;
	(void)arg5;

	core_local_storage_t *cls = get_core_local_storage();
	task_t *task = cls ? cls->task : NULL;
	if (task)
	{
		task->retVal = arg0;
	}

	task_exit();
	__builtin_unreachable();
}

// Raise SIGPIPE on the calling task: it wrote to a pipe whose readers have all
// closed, i.e. it is producing into the void. DEFAULT ACTION IS TERMINATE, and
// that default is load-bearing — it is precisely what makes `yes | head -1`
// exit instead of spinning forever. We record the signal and then end the task
// through the normal exit path (retVal carries the signal so a waiting parent
// can see HOW the child died, not just that it did).
//
// A program that wants to SURVIVE a vanishing reader installs a SIGPIPE
// handler (delivery exists since 2026-08-23): the write path then raises
// SIGPIPE and returns OS64_INTERRUPTED instead of dying (Codex #29,
// 2026-08-24). This function is the default action for everyone who did NOT
// install one — which is the behavior every pipeline actually wants.
// ── The terminating signals: enforcing the default action (SIGINT.md, PROC.md)
// Same "kernel enforces the default because ring 3 can't catch it" pattern as
// SIGPIPE below, same 128+signo retVal encoding for a waiting parent. The bit
// is set somewhere the victim is NOT running — at the KEYSTROKE
// (console_intr_intercept, IRQ path — cat writing a huge file is not reading
// the console, so a buffered byte could never work), or by another task
// writing to /proc/<id>/ctl. The KILL happens here, at the victim's own
// syscall boundary, in its own context — free to sleep, safe to close handles,
// through the very same task_exit path a voluntary death takes. Three roads
// lead here:
//   1. the dispatcher check in _syscall_dispatch (a busy task's next syscall)
//   2. console_read returning CONSOLE_READ_INTERRUPTED (blocked on stdin)
//   3. pipe_read/pipe_write returning PIPE_ERR_INTERRUPTED (blocked on a pipe)
//
// SIGKILL and SIGINT both terminate today (nothing in ring 3 can catch either
// yet) but they are NOT the same event, and the exit status must not pretend
// they are: a task killed through ctl did not die "interrupted from the
// keyboard". SIGKILL wins when both are pending — the uncatchable one always
// outranks the catchable one.
// `thread` may be NULL: all four call sites run in the VICTIM'S OWN context
// (that is the whole design), so the core's current thread is the right one
// to ask which bit is pending.
// Will something CATCH the terminate this thread is carrying? Asked by every
// blocking call before it applies the default action, so a program that
// installed a handler is told its wait was interrupted instead of being
// executed on the way to the signal it asked for.
//
// THE TASK COMES FROM THE THREAD. cls->task and cls->currentThread describe
// one thing but are two stores, and a decision made on the wrong one killed a
// program with a handler installed (2026-08-23 — see scheduler_load_thread,
// where the window itself is now closed). thread->ownerTask cannot lag,
// because it is part of the thread.
static bool current_thread_will_catch(void)
{
	core_local_storage_t *c = get_core_local_storage();
	thread_t *th = c ? c->currentThread : NULL;
	if (th == NULL)
		return false;
	return signal_has_handler_for_pending((task_t *)th->ownerTask, th);
}

static void raise_terminating_signal_and_die(task_t *task, thread_t *thread)
{
	if (thread == NULL)
	{
		core_local_storage_t *cls = get_core_local_storage();
		thread = cls ? cls->currentThread : NULL;
	}

	// WHICH signal killed it, in precedence order — the uncatchable one first,
	// then the two that name an ending world, then the keyboard's. The corpse
	// wears one tag and it should be the most specific true one.
	signal_set_t pending = (thread != NULL) ? thread->signals.sigind
	                                        : (signal_set_t){0};
	const char *why = "SIGINT";
	uint64_t code = SIGNALS_EXIT_SIGINT;

	if (sigset_has(pending, SIGKILL))      { why = "SIGKILL"; code = SIGNALS_EXIT_SIGKILL; }
	else if (sigset_has(pending, SIGTERM)) { why = "SIGTERM"; code = SIGNALS_EXIT_SIGTERM; }
	else if (sigset_has(pending, SIGHUP))  { why = "SIGHUP";  code = SIGNALS_EXIT_SIGHUP;  }

	if (task != NULL)
	{
		task->retVal = code;
		printd(DEBUG_TASK, "%s: task %s terminating (exit %lu)\n",
			why, task->exename, task->retVal);
	}

	task_exit();
	__builtin_unreachable();
}

#define TASK_EXIT_SIGPIPE 141   // 128 + signal, the classic "died by signal" encoding
static void raise_sigpipe_and_die(task_t *task)
{
	if (task != NULL)
	{
		// All threads: SIGPIPE's default action is to terminate the TASK,
		// so every thread has to learn it, not just the first one.
		task_signal_all_threads(task, SIGPIPE);
		task->retVal = TASK_EXIT_SIGPIPE;
		printd(DEBUG_TASK, "SIGPIPE: task %s wrote to a pipe with no readers — terminating\n",
			task->exename);
	}

	task_exit();
	__builtin_unreachable();
}

// ── File I/O plumbing (HANDLE_FILE) ──────────────────────────────────────────
// Every actual file operation (read/write/seek — and open/close elsewhere)
// runs under kKernelPML4 via call_in_kernel_context, because the VFS bottoms
// out in NVMe/AHCI DMA structures that live in kernel-only mappings — calling
// the driver on a user CR3 faults in nvme_submit_command, the exact lesson
// spawn learned. Same rules as spawn_params_t: the params block AND the bounce
// buffer are kmalloc'd (HHDM-reachable from both address spaces); nothing the
// helper touches may live on the syscall's task-local kernel stack.
// One page per hop for read() and file-write bounce buffers. (Defined here,
// above BOTH read and write, because the file-write path below uses it too.)
#define READ_CHUNK_SIZE  (1024 * 1024)

typedef struct {
	vfs_file_t *file;
	void       *buf;       // kmalloc'd kernel bounce buffer (never a user ptr)
	size_t      len;
	long        offset;    // seek only
	int         whence;    // seek only
	volatile long result;  // bytes moved / new position, or negative
} file_io_params_t;

static void file_do_read(void *arg)
{
	file_io_params_t *p = (file_io_params_t *)arg;
	vfs_file_t *f = p->file;
	p->result = (f->fops != NULL && f->fops->read != NULL)
	                ? f->fops->read(f, p->buf, p->len) : -1;
}

static void file_do_write(void *arg)
{
	file_io_params_t *p = (file_io_params_t *)arg;
	vfs_file_t *f = p->file;
	p->result = (f->fops != NULL && f->fops->write != NULL)
	                ? f->fops->write(f, p->buf, p->len) : -1;
}

// Commit a file's written bytes AND its directory entry to the device.
// FatFs (like every FAT implementation) keeps the new length in its own
// structures and only writes the directory entry on sync or close — so
// until this runs, a file being appended to reads as ZERO BYTES to anyone
// else, which is exactly how a live log file looked empty while the daemon
// holding it was writing happily (Chris's screenshot, 2026-08-01).
static void file_do_sync(void *arg)
{
	file_io_params_t *p = (file_io_params_t *)arg;
	vfs_file_t *f = p->file;
	p->result = (f->fops != NULL && f->fops->sync != NULL)
	                ? f->fops->sync(f) : -1;
}

// The sync(1) engine's kernel-context half: no handle, no file — the VFS
// open-file registry walk reaches EVERY task's open files, which is the
// entire point (only the kernel can sync a handle some other task holds).
static void file_do_sync_all(void *arg)
{
	file_io_params_t *p = (file_io_params_t *)arg;
	p->result = (long)vfs_sync_all();
}

static void file_do_seek(void *arg)
{
	file_io_params_t *p = (file_io_params_t *)arg;
	vfs_file_t *f = p->file;

	if (f->fops == NULL || f->fops->seek == NULL ||
	    f->fops->seek(f, p->offset, p->whence) < 0)
	{
		p->result = -1;
		return;
	}

	// seek()'s contract is "return the NEW absolute position" — genuinely more
	// useful than Unix lseek's, and free: tell() already knows. (A filesystem
	// with no tell() still seeks fine; the caller just gets 0 back.)
	p->result = (f->fops->tell != NULL) ? f->fops->tell(f) : 0;
}

// The per-thread bounce block behind thread_t.syscallIOScratch: one
// file_io_params_t header followed by READ_CHUNK_SIZE data bytes, allocated on
// the thread's first read()/write() and reused for every one after. This
// replaced a kmalloc/kfree PER CALL — which, with a no-freelist allocator,
// meant a fresh zeroed page on the way in and a TLB-shootdown IPI to every
// core on the way out, per syscall. A program reading a file byte-at-a-time
// (os64_readline's pipe-safe mode) paid that toll per BYTE: the first top
// spent ~3 seconds printing 30 tasks, nearly all of it right here.
// kmalloc'd = upper half = visible under kKernelPML4 AND every task CR3,
// which is exactly what call_in_kernel_context demands of the params block
// and buffer anyway. Per-thread (not per-core) because console/pipe reads
// BLOCK while holding the data area — see the field's comment in thread.h.
// Reuse across calls is safe: a thread runs one syscall at a time, and both
// consumers (read's bounce, write's bounce) are done with the block before
// the syscall returns. Returns NULL only if the one-time allocation fails.
static file_io_params_t *syscall_io_scratch(char **data_out)
{
	core_local_storage_t *cls = get_core_local_storage();
	thread_t *thread = cls ? cls->currentThread : NULL;
	if (thread == NULL)
		return NULL;

	if (thread->syscallIOScratch == NULL)
		thread->syscallIOScratch = kmalloc(sizeof(file_io_params_t) + READ_CHUNK_SIZE);
	if (thread->syscallIOScratch == NULL)
		return NULL;

	if (data_out != NULL)
		*data_out = (char *)thread->syscallIOScratch + sizeof(file_io_params_t);
	return (file_io_params_t *)thread->syscallIOScratch;
}

// write(handle, buffer, length) — write bytes to an output handle.
//
// The handle is resolved through the CALLING TASK'S HANDLE TABLE — which is the
// entire point of handles. This function does not know or care whether handle 1
// is the console or the write end of a pipe; it asks the table and dispatches on
// the tag. That is why the shell can redirect a program's stdout into a pipeline
// without the program (or this syscall) changing by one line.
//
// Runs on the calling task's CR3 (user buffer in the lower half, console and
// pipe rings in the shared upper half — see the table comment).
#define WRITE_CHUNK_SIZE 512
static uint64_t syscall_write(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg3;
	(void)arg4;
	(void)arg5;

	const char *user_buffer = (const char*)arg1;
	size_t length = (size_t)arg2;

	core_local_storage_t *cls = get_core_local_storage();
	task_t *task = cls ? cls->task : NULL;
	if (task == NULL)
		return SYSCALL_RESULT_INVALID;

	handle_t *h = handle_get(task, (int)(int64_t)arg0);
	if (h == NULL)
		return SYSCALL_RESULT_INVALID;

	if (length == 0)
		return 0;

	switch (h->type)
	{
		case HANDLE_CONSOLE_OUT:
		case HANDLE_CONSOLE_ERR:
		{
			// Ferry through a bounded kernel chunk so an arbitrary user length
			// never maps to unbounded kernel stack use.
			//
			// The bytes go to the CALLER'S terminal (task_tty — the
			// controlling terminal, inherited at creation): husk-on-tty2's
			// children print to tty2's grid, on the glass only while tty2 is
			// focused. This is the seam the handle contract promised — the
			// program still just writes handle 1 and never asks what's
			// behind it; the routing grew, the contract didn't move.
			tty_t *tty = task_tty(task);
			char chunk[WRITE_CHUNK_SIZE];
			size_t copied = 0;
			while (copied < length)
			{
				size_t this_chunk = length - copied;
				if (this_chunk > sizeof(chunk))
					this_chunk = sizeof(chunk);

				if (!copy_user_buffer(user_buffer + copied, chunk, this_chunk))
				{
					// Report progress if some bytes already made it to the
					// console; only fail outright when nothing was written.
					return copied ? copied : SYSCALL_RESULT_BAD_USER_DATA;
				}

				tty_write(tty, chunk, this_chunk);
				copied += this_chunk;
			}
			return copied;
		}

		case HANDLE_PTY_MASTER:
		{
			// Keystrokes INTO the window (PTY.md): same bounded ferry as the
			// console case above, delivered to pty_master_write — which runs
			// the 0x03 intercept against the SLAVE (SIGINT aims at the
			// program in the window, never the terminal holding the master)
			// and rings the slave's input for its console_read.
			tty_t *slave = (tty_t *)h->object;
			char chunk[WRITE_CHUNK_SIZE];
			size_t copied = 0;
			while (copied < length)
			{
				size_t this_chunk = length - copied;
				if (this_chunk > sizeof(chunk))
					this_chunk = sizeof(chunk);

				if (!copy_user_buffer(user_buffer + copied, chunk, this_chunk))
					return copied ? copied : SYSCALL_RESULT_BAD_USER_DATA;

				// The master's write can stop SHORT when the slave's ring is
				// full (tty.h) — report how far it really got, exactly as a
				// pipe writer would. Pretending otherwise is how a paste lost
				// everything past its first 127 bytes until 2026-08-22.
				int64_t took = pty_master_write(slave, chunk, this_chunk);
				if (took < 0)
					return copied ? (int64_t)copied : took;
				copied += (size_t)took;
				if ((size_t)took < this_chunk)
					break;
			}
			return copied;
		}

		case HANDLE_PIPE_WRITE:
		{
			pipe_t *p = (pipe_t *)h->object;

			// Copy the user bytes into kernel memory FIRST, in one piece, and
			// only THEN hand them to pipe_write. Two reasons, both load-bearing:
			//
			// 1. ATOMICITY. Our rule is that a write of <= PIPE_CAPACITY lands
			//    whole. If we chunked this through a 512-byte bounce buffer, a
			//    blocked write would interleave with another writer's bytes and
			//    the guarantee would be a lie.
			// 2. FAULTS. pipe_write copies into the ring while holding the pipe
			//    spinlock with interrupts OFF. Copying from USER memory there
			//    could demand-page — and faulting inside a spinlock with IF=0 is
			//    a deadlock (the fault handler wants locks the holder still has).
			//    Faulting out here, before any lock is taken, is perfectly safe.
			//
			// A write LARGER than the capacity can never fit in the ring, so it
			// is the one case that must chunk (documented in pipe.h) — we do it
			// at capacity granularity, which keeps each chunk atomic.
			size_t written = 0;
			while (written < length)
			{
				size_t this_chunk = length - written;
				if (this_chunk > PIPE_CAPACITY)
					this_chunk = PIPE_CAPACITY;

				// The common case — a line of text, a filter's buffer — fits
				// the thread's scratch block: no allocation at all. Only a
				// write bigger than the scratch kmallocs, because atomicity
				// demands ONE piece up to PIPE_CAPACITY (see above) and that
				// can genuinely exceed READ_CHUNK_SIZE. Safe to hold the
				// scratch across pipe_write's block: it's THIS thread's.
				char *scratch_data = NULL;
				char *kbuf;
				if (this_chunk <= READ_CHUNK_SIZE &&
				    syscall_io_scratch(&scratch_data) != NULL)
					kbuf = scratch_data;
				else
				{
					kbuf = kmalloc(this_chunk);
					if (kbuf == NULL)
						return written ? written : SYSCALL_RESULT_INVALID;
				}

				if (!copy_user_buffer(user_buffer + written, kbuf, this_chunk))
				{
					if (kbuf != scratch_data)
						kfree(kbuf);
					return written ? written : SYSCALL_RESULT_BAD_USER_DATA;
				}

				long n = pipe_write(p, kbuf, this_chunk);   // BLOCKS if full
				if (kbuf != scratch_data)
					kfree(kbuf);

				if (n == PIPE_ERR_CLOSED)
				{
					// Nobody is left to read this. If the writer INSTALLED a
					// SIGPIPE handler, honor it (Codex #29): the API accepts
					// one, so terminating anyway would make catchability a
					// lie. Raise SIGPIPE — delivery arms on this syscall's
					// exit, exactly like the terminating signals above — and
					// tell the caller its write was cut short. OS64_INTERRUPTED
					// is the arc's one answer for "a signal interrupted your
					// call"; a program that wants EPIPE-style detail inspects
					// after its handler runs. Without a handler the default
					// stands: terminate, which is what `yes | head` needs.
					if (task->sighandler[SIGPIPE] != NULL)
					{
						task_signal_all_threads(task, SIGPIPE);
						return (uint64_t)(int64_t)OS64_INTERRUPTED;
					}
					raise_sigpipe_and_die(task);
					__builtin_unreachable();
				}
				if (n == PIPE_ERR_INTERRUPTED)
				{
					// Ctrl+C landed while blocked on (or headed into) this
					// pipe write. Same rail, different signal: terminate, 130.
					// Caught? Then the wait was INTERRUPTED, not fatal — and
					// returning is what makes delivery possible at all: the
					// handler is armed on the way out of this syscall.
					if (current_thread_will_catch())
						return (uint64_t)(int64_t)OS64_INTERRUPTED;
					raise_terminating_signal_and_die(task, NULL);
					__builtin_unreachable();
				}
				if (n < 0)
					return written ? written : SYSCALL_RESULT_INVALID;

				written += (size_t)n;
			}
			return written;
		}

		case HANDLE_NET_TCP:
		{
			// A stream write: no atomicity promise and no size limit —
			// tcp_conn_write segments it and blocks until every byte is
			// acknowledged. Bounce through kernel memory in MSS-ish
			// chunks for the same fault discipline as every other write.
			size_t written = 0;
			while (written < length)
			{
				size_t this_chunk = length - written;
				if (this_chunk > READ_CHUNK_SIZE)
					this_chunk = READ_CHUNK_SIZE;

				char *kbuf = kmalloc(this_chunk);
				if (kbuf == NULL)
					return written ? written : SYSCALL_RESULT_INVALID;
				if (!copy_user_buffer(user_buffer + written, kbuf, this_chunk))
				{
					kfree(kbuf);
					return written ? written : SYSCALL_RESULT_BAD_USER_DATA;
				}

				long n = tcp_conn_write((tcp_conn_t *)h->object, kbuf, this_chunk);
				kfree(kbuf);

				if (n == TCP_ERR_INTERRUPTED)
				{
					// Caught? Then the wait was INTERRUPTED, not fatal — and
					// returning is what makes delivery possible at all: the
					// handler is armed on the way out of this syscall.
					if (current_thread_will_catch())
						return (uint64_t)(int64_t)OS64_INTERRUPTED;
					raise_terminating_signal_and_die(task, NULL);
					__builtin_unreachable();
				}
				if (n < 0)
					return written ? written : SYSCALL_RESULT_INVALID;
				written += (size_t)n;
				if ((size_t)n < this_chunk)
					break;   // connection died mid-write: report progress
			}
			return written;
		}

		case HANDLE_NET_ICMP:
		{
			// One write = ONE echo request carrying these bytes. Same
			// atomic-datagram contract as UDP; the payload comes home
			// echoed, which is the whole mechanism `ping` measures with.
			if (length > ICMP_CONN_MAX_PAYLOAD)
				return SYSCALL_RESULT_INVALID;

			char *kbuf = kmalloc(length);
			if (kbuf == NULL)
				return SYSCALL_RESULT_INVALID;
			if (!copy_user_buffer(user_buffer, kbuf, length))
			{
				kfree(kbuf);
				return SYSCALL_RESULT_BAD_USER_DATA;
			}
			long n = icmp_conn_write((icmp_conn_t *)h->object, kbuf, length);
			kfree(kbuf);
			// Unreachable neighbor = TIMEOUT (nobody answered ARP in 500ms)
			// — distinct from a malformed write, same word read() uses.
			if (n == ICMP_CONN_ERR_TIMEOUT)
				return (uint64_t)(int64_t)OS64_NET_ERR_TIMEOUT;
			return (n < 0) ? SYSCALL_RESULT_INVALID : (uint64_t)n;
		}

		case HANDLE_NET_UDP:
		{
			// One write = ONE datagram, atomic by protocol: an oversize
			// write is an ERROR, never a fragmenting loop (os64/net.h
			// states the contract; UDP_CONN_MAX_DGRAM is the physics).
			// Kernel-bounce first for the same fault-discipline as pipes.
			if (length > UDP_CONN_MAX_DGRAM)
				return SYSCALL_RESULT_INVALID;

			char *kbuf = kmalloc(length);
			if (kbuf == NULL)
				return SYSCALL_RESULT_INVALID;
			if (!copy_user_buffer(user_buffer, kbuf, length))
			{
				kfree(kbuf);
				return SYSCALL_RESULT_BAD_USER_DATA;
			}

			long n = udp_conn_write((udp_conn_t *)h->object, kbuf, length);
			kfree(kbuf);
			// Same mapping as the ICMP case: an unreachable neighbor is
			// TIMEOUT, not a generic refusal.
			if (n == UDP_CONN_ERR_TIMEOUT)
				return (uint64_t)(int64_t)OS64_NET_ERR_TIMEOUT;
			return (n < 0) ? SYSCALL_RESULT_INVALID : (uint64_t)n;
		}

		case HANDLE_FILE:
		{
			// Ferry through the thread's bounce block, chunk by chunk. Unlike
			// the pipe path there is no cross-writer atomicity promise to
			// keep — a file write that lands in pieces is still one write —
			// so chunking costs nothing but loop iterations. The scratch is
			// kmalloc'd (once, at first use): file_do_write runs under
			// kKernelPML4, which cannot see this syscall's stack.
			char *kbuf = NULL;
			file_io_params_t *fp = syscall_io_scratch(&kbuf);
			if (fp == NULL)
				return SYSCALL_RESULT_INVALID;

			size_t written = 0;
			while (written < length)
			{
				size_t this_chunk = length - written;
				if (this_chunk > READ_CHUNK_SIZE)
					this_chunk = READ_CHUNK_SIZE;

				// Copy from user space HERE, on the caller's CR3, where the
				// user buffer actually resolves (and where a demand-page fault
				// is safe — no locks held, kernel context not yet entered).
				if (!copy_user_buffer(user_buffer + written, kbuf, this_chunk))
					return written ? written : SYSCALL_RESULT_BAD_USER_DATA;

				fp->file = (vfs_file_t *)h->object;
				fp->buf = kbuf;
				fp->len = this_chunk;
				fp->result = -1;
				call_in_kernel_context(file_do_write, fp);

				if (fp->result < 0)
					return written ? written : SYSCALL_RESULT_INVALID;

				written += (size_t)fp->result;
				if ((size_t)fp->result < this_chunk)
					break;   // short write: filesystem/device is full — report progress
			}
			return written;
		}

		default:
			// Reading-only handle (or the console's input side): not writable.
			return SYSCALL_RESULT_INVALID;
	}
}

// read(handle, buffer, length) — read input bytes into a user buffer.
//
// Resolved through the calling task's handle table, exactly like write(): this
// syscall does not know whether handle 0 is the keyboard or the read end of a
// pipe. Both BLOCK (via SIGSLEEP, so the thread genuinely sleeps at zero CPU
// while other threads run) and both return SHORT — whatever is available right
// now, not a filled buffer. A pipe read returns 0 at EOF, which is what a
// filter uses to know its input is finished.
//
// Runs on the caller's CR3 (the user buffer, lower half, is directly writable);
// results are ferried through a kernel bounce buffer (READ_CHUNK_SIZE per hop).
static uint64_t syscall_read(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg4;
	(void)arg5;

	void *user_buffer = (void*)arg1;
	size_t length = (size_t)arg2;
	// arg3 = the read's PATIENCE in MILLISECONDS (contract at SYSCALL_READ in
	// the abi header, ruled 2026-08-05): 0 = poll, N = deadline,
	// OS64_WAIT_FOREVER = block — what os64_read passes. This began as
	// ping's demand made syscall ("read, but I refuse to wait past X"); the
	// original spelling made 0 mean forever, which was SO_RCVTIMEO's wart —
	// zero's one honest meaning was unsayable — and the userland branch's
	// console poll (top's 'q') forced the ruling both branches now share.
	// Writing the SAME contract into both branches on the same day is what
	// made this merge reconcile code instead of law.
	// NOTE the stub contract: os64_read/os64_read_for ALWAYS set this
	// register — it used to be ring-3 garbage the handler ignored, and
	// reading garbage as a deadline would give every old binary a random
	// patience.
	uint64_t timeout_ms = arg3;

	core_local_storage_t *cls = get_core_local_storage();
	task_t *task = cls ? cls->task : NULL;
	if (task == NULL)
		return SYSCALL_RESULT_INVALID;

	handle_t *h = handle_get(task, (int)(int64_t)arg0);
	if (h == NULL)
		return SYSCALL_RESULT_INVALID;

	if (length == 0)
		return 0;

	// Lower the deadline onto the tick clock once, here, so the console and
	// conn code below think only in ticks (sleep()'s ms→ticks doctrine).
	// Rounded UP: a 1ms deadline is a short wait, never "already expired" —
	// while 0ms stays exactly "already expired", which IS the poll gait
	// (every honoring path checks for data BEFORE the clock, so a poll still
	// delivers whatever already arrived).
	//
	// THE HONOR ROLL, joined at the merge of 2026-08-05: the console (grown
	// on the userland branch for top's 'q') AND the three dialed net handles
	// (grown on the net branch for ping's patience). Two branches solved the
	// same problem for different consumers in the same week; this list is
	// where they meet. Everywhere else a finite timeout is REFUSED rather
	// than silently ignored — the tripwire doctrine: a pipe read that accepts
	// a patience it won't keep is a lie with a delay. Pipes and files grow it
	// the day a real consumer demands it there.
	uint64_t deadline = 0;
	if (timeout_ms != OS64_WAIT_FOREVER)
	{
		if (h->type != HANDLE_CONSOLE_IN &&
		    h->type != HANDLE_NET_UDP && h->type != HANDLE_NET_TCP &&
		    h->type != HANDLE_NET_ICMP)
			return SYSCALL_RESULT_INVALID;
		deadline = kTicksSinceStart + (timeout_ms + MS_PER_TICK - 1) / MS_PER_TICK;
	}

	size_t want = length < READ_CHUNK_SIZE ? length : READ_CHUNK_SIZE;
	// The thread-owned bounce block (see syscall_io_scratch): no allocation,
	// no free, no per-call TLB shootdown — and therefore no kfree on ANY exit
	// path below, which is why the error paths got shorter.
	char *kbuf = NULL;
	file_io_params_t *fp = syscall_io_scratch(&kbuf);
	if (fp == NULL)
		return SYSCALL_RESULT_INVALID;

	long got = 0;
	switch (h->type)
	{
		case HANDLE_CONSOLE_IN:
			// Blocks until >=1 byte is available (terminal semantics) — or
			// until the caller's patience runs out, when there is any. The
			// scratch is safe to hold across the block — it's THIS thread's.
			got = console_read_deadline(kbuf, want, deadline);
			if (got == CONSOLE_READ_INTERRUPTED)
			{
				// Ctrl+C landed while (or before) we were blocked on stdin.
				// Default action: terminate. The sentinel never reaches ring 3.
				// Caught? Then the wait was INTERRUPTED, not fatal — and
				// returning is what makes delivery possible at all: the
				// handler is armed on the way out of this syscall.
				if (current_thread_will_catch())
					return (uint64_t)(int64_t)OS64_INTERRUPTED;
				raise_terminating_signal_and_die(task, NULL);
				__builtin_unreachable();
			}
			if (got == CONSOLE_READ_TIMEOUT)
			{
				// The deadline expired byteless. The kernel sentinel stays
				// kernel-side; ring 3 gets the ABI's named verdict — a value
				// no other read outcome shares, so "nothing yet" can never
				// impersonate EOF's 0 (the V7 O_NDELAY sin, refused).
				return (uint64_t)(int64_t)OS64_ERR_TIMEOUT;
			}
			break;

		case HANDLE_PTY_MASTER:
			// The teaching error PTY.md promised: GRID mode has no byte
			// stream — the screen comes out through pty_snapshot. read()
			// takes on meaning when the STREAM flavor arrives (its gate is
			// TCP listen(); its customer is telnetd).
			printd(DEBUG_SYSCALL,
			       "read(pty master): GRID mode has no byte stream — use pty_snapshot\n");
			return SYSCALL_RESULT_INVALID;

		case HANDLE_PIPE_READ:
			// Blocks until >=1 byte is available, OR the last writer closes —
			// which returns 0, and 0 is EOF. (EOF is the absence of writers.)
			// Copying into a KERNEL buffer, not straight to user space, for the
			// same reason write() does the reverse: pipe_read copies under the
			// pipe spinlock with interrupts off, and touching user memory there
			// could demand-page into a deadlock.
			got = pipe_read((pipe_t *)h->object, kbuf, want);
			if (got == PIPE_ERR_INTERRUPTED)
			{
				// Ctrl+C landed while blocked on (or headed into) a pipe read.
				// Caught? Then the wait was INTERRUPTED, not fatal — and
				// returning is what makes delivery possible at all: the
				// handler is armed on the way out of this syscall.
				if (current_thread_will_catch())
					return (uint64_t)(int64_t)OS64_INTERRUPTED;
				raise_terminating_signal_and_die(task, NULL);
				__builtin_unreachable();
			}
			break;

		case HANDLE_NET_TCP:
			// A stream read: blocks for at least one byte, returns SHORT,
			// and returns 0 at EOF once the peer's FIN drains — the exact
			// contract a pipe read has, which is why `cat` over a socket
			// would need no new code at all.
			// (No kfree on ANY path here: kbuf is the thread-owned scratch,
			// and worse, an INTERIOR pointer into it — the kfrees that used
			// to sit on these exits were a fossil from when this buffer was
			// a per-call kmalloc, armed to wild-free on the first Ctrl+C.)
			got = tcp_conn_read((tcp_conn_t *)h->object, kbuf, want, deadline);
			if (got == TCP_ERR_INTERRUPTED)
			{
				// Caught? Then the wait was INTERRUPTED, not fatal — and
				// returning is what makes delivery possible at all: the
				// handler is armed on the way out of this syscall.
				if (current_thread_will_catch())
					return (uint64_t)(int64_t)OS64_INTERRUPTED;
				raise_terminating_signal_and_die(task, NULL);
				__builtin_unreachable();
			}
			if (got == TCP_ERR_TIMEOUT)
				return (uint64_t)(int64_t)OS64_NET_ERR_TIMEOUT;
			if (got < 0)
			{
				// RST or a dead connection — distinct from EOF's clean 0.
				return SYSCALL_RESULT_INVALID;
			}
			break;

		case HANDLE_NET_ICMP:
			// Blocks until an echo reply carrying OUR identifier comes
			// back, then returns the payload we sent, as the peer echoed
			// it. (What `ping` does with those bytes — a timestamp,
			// a pattern check — is `ping`'s business.)
			got = icmp_conn_read((icmp_conn_t *)h->object, kbuf, want, deadline);
			if (got == ICMP_CONN_ERR_INTERRUPTED)
			{
				// Caught? Then the wait was INTERRUPTED, not fatal — and
				// returning is what makes delivery possible at all: the
				// handler is armed on the way out of this syscall.
				if (current_thread_will_catch())
					return (uint64_t)(int64_t)OS64_INTERRUPTED;
				raise_terminating_signal_and_die(task, NULL);
				__builtin_unreachable();
			}
			if (got == ICMP_CONN_ERR_TIMEOUT)
				return (uint64_t)(int64_t)OS64_NET_ERR_TIMEOUT;
			break;

		case HANDLE_NET_UDP:
			// Blocks until one datagram arrives from the dialed peer, then
			// returns exactly that datagram (short if the buffer is smaller;
			// the tail drops — the truncation contract in os64/net.h).
			// `want` is already min(length, READ_CHUNK_SIZE=4096), which
			// exceeds UDP_CONN_MAX_DGRAM=1472, so no datagram is ever
			// clipped by the bounce buffer — only by the CALLER's length.
			got = udp_conn_read((udp_conn_t *)h->object, kbuf, want, deadline);
			if (got == UDP_CONN_ERR_INTERRUPTED)
			{
				// Ctrl+C landed while blocked waiting for a packet — same
				// rail as console and pipe reads: terminate, 130.
				// Caught? Then the wait was INTERRUPTED, not fatal — and
				// returning is what makes delivery possible at all: the
				// handler is armed on the way out of this syscall.
				if (current_thread_will_catch())
					return (uint64_t)(int64_t)OS64_INTERRUPTED;
				raise_terminating_signal_and_die(task, NULL);
				__builtin_unreachable();
			}
			if (got == UDP_CONN_ERR_TIMEOUT)
				return (uint64_t)(int64_t)OS64_NET_ERR_TIMEOUT;
			break;

		case HANDLE_THREAD:
		{
			// Reading a thread blocks until it finishes and yields its
			// return value — an int64_t, nothing more. A short buffer is
			// a caller bug rather than a partial answer, because half a
			// return value means nothing.
			//
			// THE TWO kfree(kbuf) CALLS THAT USED TO SIT ON THESE EXITS
			// ARE GONE, removed at the merge of 2026-08-05 and worth the
			// paragraph: kbuf is the THREAD-OWNED scratch block, and an
			// INTERIOR pointer into it at that (the data area sits after
			// the params struct), so freeing it hands the allocator an
			// address it never issued. The net branch had already swept
			// this exact fossil out of its own read cases — a leftover
			// from when the bounce buffer was a per-call kmalloc — and
			// left a comment saying so; userland's copy survived only
			// because neither exit had ever been taken. A short buffer or
			// a Ctrl+C on a thread read would have found it.
			if (want < sizeof(int64_t))
				return SYSCALL_RESULT_INVALID;
			int64_t retval = 0;
			long jr = thread_join_read((thread_join_t *)h->object, &retval);
			if (jr == THREAD_JOIN_ERR_INTERRUPTED)
			{
				// Caught? Then the wait was INTERRUPTED, not fatal — and
				// returning is what makes delivery possible at all: the
				// handler is armed on the way out of this syscall.
				if (current_thread_will_catch())
					return (uint64_t)(int64_t)OS64_INTERRUPTED;
				raise_terminating_signal_and_die(task, NULL);
				__builtin_unreachable();
			}
			memcpy(kbuf, &retval, sizeof(retval));
			got = (long)sizeof(retval);
			break;
		}

		case HANDLE_FILE:
		{
			// Never blocks (a file always knows its bytes) and returns SHORT at
			// the end: fewer bytes than asked near EOF, then 0 AT EOF — so the
			// canonical filter loop works on a file with zero special-casing.
			// The actual read runs under kKernelPML4 (see file_do_read); the
			// scratch block is kmalloc'd (once), reachable from both worlds.
			fp->file = (vfs_file_t *)h->object;
			fp->buf = kbuf;
			fp->len = want;
			fp->result = -1;
			call_in_kernel_context(file_do_read, fp);
			got = fp->result;

			if (got < 0)
			{
				// A real device/filesystem error — distinct from EOF's clean 0.
				return SYSCALL_RESULT_INVALID;
			}
			break;
		}

		default:
			return SYSCALL_RESULT_INVALID;   // a write-only handle
	}

	if (got <= 0)
		return 0;   // EOF (pipe) or nothing (console)

	// THE TRUST BOUNDARY (added 2026-08-10). Every case above got `want` as its
	// limit and handed back a count, and until now the only thing asked of that
	// count was that it not be negative. It is about to become the LENGTH OF A
	// WRITE INTO RING-3 MEMORY — so a driver that returns more than it was given
	// does not produce a wrong answer, it produces a buffer overrun in whatever
	// called read(), which for the common `char line[N]` idiom means somebody's
	// stack. The count also over-reads kbuf on the way out, so the bytes doing
	// the smashing are whatever happened to be in the bounce block.
	//
	// This is NOT the "clamp everything, everywhere" reflex — it is the one
	// place where an integer returned by arbitrary filesystem code turns into a
	// memory write in another privilege level, and the house has already been
	// bitten by exactly this class once: nvme_vfs_write_disk was `void` behind a
	// (void*) cast, returning whatever RAX held, and the fingerprint it left is
	// still in SUCCESSION.md ("Root filesystem disk test failed: 4294967291").
	// Every filesystem in the tree is audited clean today (synthfs clamps to its
	// remaining bytes, and userland's readline is airtight); this makes the next
	// one that isn't a bounded short read instead of a stack smash.
	//
	// Clamped rather than fatal ON PURPOSE: the clamp already makes it safe, and
	// turning a survivable driver bug into a dead machine inside read() — the
	// syscall EVERYTHING uses — is a bad trade. It is loud instead. If Chris
	// would rather this panic, it is a one-line change.
	if ((size_t)got > want)
	{
		printd(DEBUG_VFS | DEBUG_SYSCALL,
		       "read: HANDLE TYPE %d RETURNED %ld BYTES FOR A %lu-BYTE REQUEST — "
		       "driver contract violated, clamping (this would have overrun the "
		       "caller's buffer)\n",
		       (int)h->type, got, (unsigned long)want);
		got = (long)want;
	}

	if (!copy_to_user_buffer(user_buffer, kbuf, (size_t)got))
		return SYSCALL_RESULT_BAD_USER_DATA;

	return (uint64_t)got;
}

// pipe(int out[2]) — create a pipe; out[0] = read end, out[1] = write end.
//
// The creator ends up holding BOTH ends (readers == writers == 1). A shell
// hands one end to each child (spawn refs them) and then MUST close its own two
// copies — if it doesn't, the writer count never reaches zero, the reader never
// sees EOF, and `a | b` hangs forever. That is the single most common way a
// hand-written shell breaks, so it is worth saying twice.
static uint64_t syscall_pipe(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5;

	core_local_storage_t *cls = get_core_local_storage();
	task_t *task = cls ? cls->task : NULL;
	if (task == NULL)
		return SYSCALL_RESULT_INVALID;

	pipe_t *p = pipe_create();
	if (p == NULL)
		return SYSCALL_RESULT_INVALID;

	int rh = handle_alloc(task, HANDLE_PIPE_READ, p);
	int wh = handle_alloc(task, HANDLE_PIPE_WRITE, p);
	if (rh < 0 || wh < 0)
	{
		// Out of handles — unwind cleanly. Close whichever end we DID install
		// via the table (so its refcount drops), and the raw end we didn't.
		if (rh >= 0) handle_close(task, rh); else pipe_close_read_end(p);
		if (wh >= 0) handle_close(task, wh); else pipe_close_write_end(p);
		return SYSCALL_RESULT_INVALID;
	}

	int handles[2] = { rh, wh };
	if (!copy_to_user_buffer((void *)arg0, handles, sizeof(handles)))
	{
		handle_close(task, rh);
		handle_close(task, wh);
		return SYSCALL_RESULT_BAD_USER_DATA;
	}

	printd(DEBUG_PIPE, "pipe: task %s got handles r=%d w=%d\n", task->exename, rh, wh);
	return 0;
}

// The cell layouts must be the SAME BYTES — pty_snapshot memcpys grid rows
// straight into the ABI struct. Pinned the ext2-superblock way: change either
// side and the build stops here instead of the terminal rendering confetti.
_Static_assert(sizeof(tty_cell_t) == sizeof(os64_pty_cell_t),
               "pty cell ABI drifted from tty_cell_t (size)");
_Static_assert(__builtin_offsetof(tty_cell_t, color) ==
               __builtin_offsetof(os64_pty_cell_t, color),
               "pty cell ABI drifted from tty_cell_t (color offset)");

// pty_create(cols, rows) -> master handle. The pipe handler above is this
// function's template; the difference is one object and one end — a pty has
// no slave handle at all (PTY.md: the slave is named THROUGH the master at
// spawn, and by task->tty everywhere else).
static uint64_t syscall_pty_create(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg2; (void)arg3; (void)arg4; (void)arg5;

	core_local_storage_t *cls = get_core_local_storage();
	task_t *task = cls ? cls->task : NULL;
	if (task == NULL)
		return SYSCALL_RESULT_INVALID;

	tty_t *slave = pty_create_slave((uint32_t)arg0, (uint32_t)arg1);
	if (slave == NULL)
		return SYSCALL_RESULT_INVALID;

	int mh = handle_alloc(task, HANDLE_PTY_MASTER, slave);
	if (mh < 0)
	{
		// Out of handles: a never-seated slave with a closed master is
		// exactly the burial condition — close does the whole unwind.
		pty_master_close(slave);
		return SYSCALL_RESULT_INVALID;
	}

	printd(DEBUG_SYSCALL, "pty_create: task %s got master %d (pty%u, %ux%u)\n",
	       task->exename, mh, slave->index, slave->cols, slave->rows);
	return (uint64_t)mh;
}

// pty_snapshot(master, header out, cells out, max_cells) -> cells copied.
// max_cells == 0 is the cheap poll: header only (generation + HUNGUP), no
// cell traffic — what a terminal calls at frame cadence. The full copy goes
// through a kernel scratch, NOT straight to user memory: the linearization
// walks the grid under t->lock (irqsave), and touching user memory there
// could demand-page inside a spinlock — the same discipline pipe read/write
// document at length.
static uint64_t syscall_pty_snapshot(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg4; (void)arg5;

	core_local_storage_t *cls = get_core_local_storage();
	task_t *task = cls ? cls->task : NULL;
	if (task == NULL)
		return SYSCALL_RESULT_INVALID;

	handle_t *h = handle_get(task, (int)(int64_t)arg0);
	if (h == NULL || h->type != HANDLE_PTY_MASTER)
		return SYSCALL_RESULT_INVALID;
	tty_t *t = (tty_t *)h->object;

	uint32_t maxCells = (uint32_t)arg3;
	uint32_t want = t->cols * t->rows;
	uint32_t ncells = (maxCells < want) ? maxCells : want;
	if (ncells > 0 && arg2 == 0)
		return SYSCALL_RESULT_BAD_USER_DATA;   // asked for cells, gave no bucket

	os64_pty_cell_t *scratch = NULL;
	if (ncells > 0)
	{
		scratch = kmalloc((size_t)ncells * sizeof(*scratch));
		if (scratch == NULL)
			return SYSCALL_RESULT_INVALID;
	}

	os64_pty_header_t hdr;
	uint64_t flags = spinlock_acquire_irqsave(&t->lock);
	hdr.cols       = t->cols;
	hdr.rows       = t->rows;
	hdr.cur_row    = t->cur_row;
	hdr.cur_col    = t->cur_col;
	hdr.generation = t->generation;
	hdr.flags      = (t->everSeated && t->seats <= 0) ? OS64_PTY_HUNGUP : 0;
	hdr._reserved  = 0;
	if (ncells > 0)
	{
		// Linearize the LIVE screen out of the scrollback ring: visual row r
		// lives at ring line (screen_top + r) % total_lines. The copy is
		// legal as one memcpy per row because the static asserts above pin
		// the two cell layouts together.
		uint32_t copied = 0;
		for (uint32_t r = 0; r < t->rows && copied < ncells; r++)
		{
			uint32_t line = (t->screen_top + r) % t->total_lines;
			const tty_cell_t *src = &t->cells[(size_t)line * t->cols];
			uint32_t n = t->cols;
			if (copied + n > ncells)
				n = ncells - copied;
			memcpy(&scratch[copied], src, (size_t)n * sizeof(tty_cell_t));
			copied += n;
		}
		ncells = copied;
	}
	spinlock_release_irqrestore(&t->lock, flags);

	if (!copy_to_user_buffer((void *)arg1, &hdr, sizeof(hdr)))
	{
		if (scratch != NULL)
			kfree(scratch);
		return SYSCALL_RESULT_BAD_USER_DATA;
	}
	if (ncells > 0)
	{
		bool ok = copy_to_user_buffer((void *)arg2, scratch,
		                              (size_t)ncells * sizeof(os64_pty_cell_t));
		kfree(scratch);
		if (!ok)
			return SYSCALL_RESULT_BAD_USER_DATA;
	}
	return ncells;
}

// net_dial(dest) — open a network conversation and hand back a handle.
//
// The whole ratified API doctrine lands in this one handler: a TYPED STRUCT
// crosses the boundary (never a dial string — libos64 parses those, ruling
// #1), every field HOST-order (ruling #2), and what comes back is an
// ordinary handle whose read/write ARE the datagram verbs (ruling #4) —
// dispatched by the same switches that serve pipes and files, because a
// conversation is just one more thing a handle can name.
static uint64_t syscall_net_dial(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5;

	core_local_storage_t *cls = get_core_local_storage();
	task_t *task = cls ? cls->task : NULL;
	if (task == NULL)
		return SYSCALL_RESULT_INVALID;

	os64_netdest_t dest;
	if (!copy_user_buffer((const void *)arg0, &dest, sizeof(dest)))
		return SYSCALL_RESULT_BAD_USER_DATA;

	// Refusals from here down are SPECIFIC (the OS64_NET_ERR_* table in
	// os64/net.h): a caller who dialed and got "no" is owed the reason —
	// a mangled struct, a netless boot, and a peer that said RST are three
	// different next moves, and collapsing them into -1 turns every one
	// of them into a debugging session.
	if (dest.ip == 0)
		return (uint64_t)(int64_t)OS64_NET_ERR_BAD_DEST;
	if (dest.protocol != OS64_NET_UDP && dest.protocol != OS64_NET_TCP &&
	    dest.protocol != OS64_NET_ICMP)
		return (uint64_t)(int64_t)OS64_NET_ERR_BAD_DEST;   // no wildcard: os64 says what it means
	// A port of 0 is meaningless for the port protocols; ICMP has no ports
	// at all (its identifier is kernel-assigned), so the field is ignored.
	if (dest.port == 0 && dest.protocol != OS64_NET_ICMP)
		return (uint64_t)(int64_t)OS64_NET_ERR_BAD_DEST;

	// Dial tone requires a line: no NIC, no conversation. (A netless boot
	// is a configuration, not an error — but dialing on one is an error.)
	if (kNetDeviceCount == 0)
		return (uint64_t)(int64_t)OS64_NET_ERR_NO_NIC;

	// One syscall, two protocols, two object types behind two handle tags —
	// and from the caller's side the difference shows up only in what
	// read/write MEAN (datagrams vs bytes), exactly as ruling #1 intended
	// when it made the protocol segment the verb of the call.
	void *conn;
	handle_type_t tag;
	// UDP and ICMP dials touch no wire — their only way to fail is running
	// out of something (memory, ports, identifiers). TCP actually converses,
	// so its dial carries the extra answers (REFUSED/TIMEOUT) back in `why`.
	int64_t why = OS64_NET_ERR_NO_RESOURCES;
	if (dest.protocol == OS64_NET_TCP)
	{
		// BLOCKS through the three-way handshake (or a refusal, or the
		// connect timeout) — a dial that returns has a live stream.
		conn = tcp_conn_dial(kNetDevices[0], dest.ip, dest.port, &why);
		tag = HANDLE_NET_TCP;
	}
	else if (dest.protocol == OS64_NET_ICMP)
	{
		// Nothing to negotiate: echo has no handshake, so the dial only
		// allocates the identifier that makes replies findable.
		conn = icmp_conn_dial(kNetDevices[0], dest.ip);
		tag = HANDLE_NET_ICMP;
	}
	else
	{
		conn = udp_conn_dial(kNetDevices[0], dest.ip, dest.port);
		tag = HANDLE_NET_UDP;
	}
	if (conn == NULL)
		return (uint64_t)why;

	int h = handle_alloc(task, tag, conn);
	if (h < 0)
	{
		// Out of handle slots — hang up cleanly on whichever we opened.
		if      (tag == HANDLE_NET_TCP)  tcp_conn_close((tcp_conn_t *)conn);
		else if (tag == HANDLE_NET_ICMP) icmp_conn_close((icmp_conn_t *)conn);
		else                             udp_conn_close((udp_conn_t *)conn);
		return (uint64_t)(int64_t)OS64_NET_ERR_NO_RESOURCES;
	}

	const char *proto_name = (dest.protocol == OS64_NET_TCP)  ? "tcp" :
	                         (dest.protocol == OS64_NET_ICMP) ? "icmp" : "udp";
	printd(DEBUG_NET, "net_dial: task %s -> %s!%u.%u.%u.%u!%u = handle %d\n",
	       task->exename, proto_name, NET_IPV4_OCTETS(dest.ip), dest.port, h);
	return (uint64_t)h;
}

// close(handle) — give up one handle.
//
// For a pipe end this is not bookkeeping, it is SIGNALLING: dropping the last
// write end is what delivers EOF to the reader, and dropping the last read end
// is what delivers SIGPIPE to the writer. close() is how a program says "I am
// done with this end" — and in a pipeline, saying so is mandatory.
static uint64_t syscall_close(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5;

	core_local_storage_t *cls = get_core_local_storage();
	task_t *task = cls ? cls->task : NULL;
	if (task == NULL)
		return SYSCALL_RESULT_INVALID;

	if (!handle_close(task, (int)(int64_t)arg0))
		return SYSCALL_RESULT_INVALID;

	return 0;
}

// ── open / seek ──────────────────────────────────────────────────────────────

// Everything open's kernel-context helper needs, in one kmalloc'd (HHDM)
// block — spawn_params_t's little sibling. path_copy is special: the VFS
// stores the path POINTER it is given (f_path), so open must hand it a string
// with HANDLE lifetime, not syscall lifetime. handle_file_object_close frees
// it when the handle dies; this block itself dies with the syscall.
typedef struct {
	char        mode[4];      // "r"/"u"/"w"/"a"/"c"/"d" — validated before we get here
	char       *path_copy;    // kmalloc'd, becomes f_path, outlives the syscall
	vfs_filesystem_t *fs;     // mount-resolved BEFORE kernel context (pure
	                          // string matching); path_copy is the fs-local
	                          // TAIL, which is what the fs must see AND what
	                          // f_path must be (the handle closer kfree's
	                          // f_path — it has to be a base pointer)
	vfs_file_t *file;         // out: the opened file       (file modes)
	vfs_directory_t *dir;     // out: the opened directory  (mode "d")
	volatile long result;     // 0 on success, negative on failure
} open_params_t;

// Runs under kKernelPML4: the open walks directories, which is disk I/O.
static void open_do(void *arg)
{
	open_params_t *p = (open_params_t *)arg;

	// Mode "d" opens a DIRECTORY for readdir() — same syscall, same handle
	// table, different tag. "One handle type for everything" means opendir
	// is just open asking for a different thing back.
	if (p->mode[0] == 'd')
	{
		if (p->fs->dops == NULL || p->fs->dops->open == NULL)
		{
			p->result = -1;
			return;
		}
		p->result = p->fs->dops->open(&p->dir, p->path_copy, p->fs);
		return;
	}

	if (p->fs->fops == NULL || p->fs->fops->open == NULL)
	{
		p->result = -1;
		return;
	}

	p->result = p->fs->fops->open(&p->file, p->path_copy, p->mode, p->fs);
}

// Resolve a just-copied user path against the task's cwd into canonical
// absolute form. THE relative-path choke point: open (files AND dirs), spawn,
// and chdir all pass through here, which is what makes `cd /bin` + `hello`
// (or ls with a bare "dir1") work everywhere at once — and means no
// filesystem driver ever sees a "..". Returns false if the result overflows.
static bool resolve_user_path(task_t *task, const char *in, char *out, size_t outlen)
{
	const char *cwd = (task != NULL && task->cwd != NULL) ? task->cwd : "/";
	return vfs_canonicalize_path(cwd, in, out, outlen) == 0;
}

// readdir's kernel-context half: one dops->read per call, into the HHDM
// params block (see syscall_readdir below).
typedef struct {
	vfs_directory_t *dir;
	os64_dirent_t entry;    // out, when result == 1
	volatile long result;   // 1 = entry, 0 = end of directory, <0 = error
} readdir_params_t;

static void readdir_do(void *arg)
{
	readdir_params_t *p = (readdir_params_t *)arg;
	vfs_directory_t *d = p->dir;
	p->result = (d->dops != NULL && d->dops->read != NULL)
	                ? d->dops->read(d, &p->entry) : -1;
}

// open(path, mode) — open a file on the root filesystem, return a handle
// (>= 3, the standard streams are never displaced), or a SYSCALL_RESULT_*
// sentinel. mode is a 1-letter string, validated HERE at the boundary:
//   "r" read existing   "u" update existing without truncation
//   "w"/"c" create-or-truncate for writing   "a" append
// NULL mode means "r". The handle then plugs into read/write/seek/close —
// and into spawn's redirection slots, where `upper < file` falls out for free.
static uint64_t syscall_open(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg2; (void)arg3; (void)arg4; (void)arg5;

	const char *user_path = (const char *)arg0;
	const char *user_mode = (const char *)arg1;

	core_local_storage_t *cls = get_core_local_storage();
	task_t *task = cls ? cls->task : NULL;
	if (task == NULL)
		return SYSCALL_RESULT_INVALID;

	open_params_t *p = kmalloc(sizeof(*p));
	if (p == NULL)
		return SYSCALL_RESULT_INVALID;

	// Marshal the path into a scratch buffer first (syscall lifetime), then
	// clone it into path_copy (handle lifetime) once we know its real length.
	// Between the two: resolve against the task's cwd — open("notes.txt")
	// means "here", and "here" is kernel state now.
	char raw[TASK_MAX_PATH_LEN];
	char path[TASK_MAX_PATH_LEN];
	if (!copy_user_string(user_path, raw, sizeof(raw)))
	{
		kfree(p);
		return SYSCALL_RESULT_BAD_USER_DATA;
	}
	if (!resolve_user_path(task, raw, path, sizeof(path)))
	{
		kfree(p);
		return SYSCALL_RESULT_BAD_USER_DATA;
	}

	if (user_mode == NULL)
	{
		p->mode[0] = 'r';
		p->mode[1] = '\0';
	}
	else if (!copy_user_string(user_mode, p->mode, sizeof(p->mode)))
	{
		kfree(p);
		return SYSCALL_RESULT_BAD_USER_DATA;
	}

	// Boundary validation: exactly one known mode letter. An unrecognized mode
	// would fall through the FAT glue as access-flags 0 — an open that succeeds
	// and then can't read, which is a miserable thing to debug from ring 3.
	// ("d" = directory, for readdir — see open_do.)
	if (p->mode[1] != '\0' ||
	    (p->mode[0] != 'r' && p->mode[0] != 'u' && p->mode[0] != 'w' &&
	     p->mode[0] != 'a' && p->mode[0] != 'c' && p->mode[0] != 'd'))
	{
		kfree(p);
		return SYSCALL_RESULT_BAD_USER_DATA;
	}

	// Route the canonical path to its mounted filesystem HERE, before kernel
	// context — the resolver is pure string matching against the mount table
	// (kernel .data, visible from any CR3). What gets cloned below is the
	// fs-local TAIL: the fs stores that pointer as f_path and the handle
	// closer kfree's it, so it must be a base pointer, never path+offset.
	const char *tail = NULL;
	p->fs = vfs_resolve_mount(path, &tail);
	if (p->fs == NULL)
	{
		kfree(p);
		return SYSCALL_RESULT_INVALID;   // nothing mounted yet
	}

	// THE HANDLE ALIAS (devfs, 2026-08-20). One path in the whole namespace —
	// /dev/tty — names a HANDLE KIND rather than a byte container, and it is
	// answered here, before any filesystem is asked to open anything.
	//
	// It has to happen at this layer: a console handle carries no object and
	// late-binds to task_tty(caller) at every read, and the read itself
	// BLOCKS. A fops->read could not do it — HANDLE_FILE reads run through
	// call_in_kernel_context on the core's interrupt stack, and sleeping there
	// is how this kernel gets corrupted (the stack poisoner caught exactly
	// that on 2026-08-13). Answering at open costs one branch and reuses the
	// entire console path downstream: Ctrl+C, EOF, tty focus, all unchanged.
	//
	// This is the promise the comment at syscall_tty_handle made — the verb
	// came first because a pager needed it before a devfs existed, and the
	// name now NAMES the verb instead of rivalling it. Both spellings mint the
	// same handle; only one of them can be written in husk.rc.
	handle_type_t alias_type;
	if (devfs_handle_alias(p->fs, tail, p->mode, &alias_type))
	{
		kfree(p);
		int ah = handle_alloc(task, alias_type, NULL);
		if (ah < 0)
			return SYSCALL_RESULT_INVALID;   // table full — the only failure here
		printd(DEBUG_SYSCALL, "open: task %s: '%s' aliased to handle %d\n",
		       task->exename, path, ah);
		return (uint64_t)ah;
	}

	size_t plen = 0;
	while (tail[plen] != '\0')
		plen++;
	p->path_copy = kmalloc(plen + 1);
	if (p->path_copy == NULL)
	{
		kfree(p);
		return SYSCALL_RESULT_INVALID;
	}
	memcpy(p->path_copy, tail, plen + 1);

	p->file = NULL;
	p->dir = NULL;
	p->result = -1;
	bool is_dir = (p->mode[0] == 'd');

	// The directory walk does disk I/O — kernel context required (see open_do).
	call_in_kernel_context(open_do, p);

	if (p->result != 0 || (is_dir ? (void *)p->dir : (void *)p->file) == NULL)
	{
		printd(DEBUG_SYSCALL, "open: task %s: '%s' mode '%s' failed\n",
		       task->exename, path, p->mode);   // full path — tail loses the mount
		kfree(p->path_copy);
		kfree(p);
		return SYSCALL_RESULT_INVALID;   // no such file/directory / bad path
	}

	int h;
	if (is_dir)
	{
		// Tag mount roots for mount-aware readdir: an fs-local path of "/"
		// means this handle opened the top of its mount, so readdir must
		// append the mount points living directly under it once the fs's
		// own entries end (see vfs_readdir_child_mounts). The prefix is
		// found by the fs pointer — one mount per fs by GUID dedupe.
		if (p->path_copy[0] == '/' && p->path_copy[1] == '\0')
			for (int i = 0; i < kMountCount; i++)
				if (kMountTable[i].fs == p->fs)
				{
					p->dir->mount_prefix = kMountTable[i].prefix;
					p->dir->mount_prefix_len = kMountTable[i].prefix_len;
					break;
				}

		// Directories carry no refcount — spawn redirection rejects them, so
		// this handle is the object's one and only owner (see handle.h).
		h = handle_alloc(task, HANDLE_DIR, p->dir);
		if (h < 0)
		{
			handle_dir_object_close(p->dir);   // also frees path_copy (f_path)
			kfree(p);
			return SYSCALL_RESULT_INVALID;
		}
	}
	else
	{
		// One handle references this file so far (see handleRefCount in
		// vfs.h) — set BEFORE handle_alloc so no close path can ever see it
		// uninitialized.
		p->file->handleRefCount = 1;

		h = handle_alloc(task, HANDLE_FILE, p->file);
		if (h < 0)
		{
			// Table full — unwind the open. This also frees path_copy (it is
			// the file's f_path now; the closer owns it from here).
			handle_file_object_close(p->file);
			kfree(p);
			return SYSCALL_RESULT_INVALID;
		}
	}

	printd(DEBUG_SYSCALL, "open: task %s: '%s' mode '%s' -> handle %d\n",
	       task->exename, path, p->mode, h);   // full path — tail loses the mount
	kfree(p);
	return (uint64_t)h;
}

// readdir(handle, entry_out) — produce the next entry of an open directory
// (a handle from open(path, "d")). Returns 1 = *entry_out filled, 0 = end of
// directory, or a SYSCALL_RESULT_* sentinel. The entry is the fs-neutral
// os64_dirent_t (abi/include/os64/dirent.h): name, size, and a DIR flag in
// one call — no per-entry stat() dance.
static uint64_t syscall_readdir(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg2; (void)arg3; (void)arg4; (void)arg5;

	core_local_storage_t *cls = get_core_local_storage();
	task_t *task = cls ? cls->task : NULL;
	if (task == NULL)
		return SYSCALL_RESULT_INVALID;

	handle_t *h = handle_get(task, (int)(int64_t)arg0);
	if (h == NULL || h->type != HANDLE_DIR)
		return SYSCALL_RESULT_INVALID;   // not a directory handle

	// Params + entry in one HHDM block: the dops->read runs under kKernelPML4
	// (directory entries come off the disk), the copy-out runs back on the
	// caller's CR3 — the block must be visible to both.
	readdir_params_t *p = kmalloc(sizeof(*p));
	if (p == NULL)
		return SYSCALL_RESULT_INVALID;
	p->dir = (vfs_directory_t *)h->object;
	p->result = -1;

	call_in_kernel_context(readdir_do, p);

	long r = p->result;

	// The fs's real entries are done: if this dir is a mount root, the mount
	// points under it come next — namespace content no on-disk filesystem
	// can know about. Pure mount-table scan, fine on the caller's CR3.
	if (r == 0)
		r = vfs_readdir_child_mounts(p->dir, &p->entry);

	if (r == 1 && !copy_to_user_buffer((void *)arg1, &p->entry, sizeof(p->entry)))
	{
		kfree(p);
		return SYSCALL_RESULT_BAD_USER_DATA;
	}
	kfree(p);

	if (r < 0)
		return SYSCALL_RESULT_INVALID;
	return (uint64_t)r;   // 1 = entry delivered, 0 = end of directory
}

// seek(handle, offset, whence) — move a file handle's position; returns the
// NEW absolute position (more useful than lseek's "whatever you passed in"),
// or a SYSCALL_RESULT_* sentinel. whence is OS64_SEEK_SET/CUR/END (the ABI
// header) which match the VFS's SEEK_* by design. Seeking a pipe or the
// console is an error — position is a property only files have.
static uint64_t syscall_seek(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg3; (void)arg4; (void)arg5;

	core_local_storage_t *cls = get_core_local_storage();
	task_t *task = cls ? cls->task : NULL;
	if (task == NULL)
		return SYSCALL_RESULT_INVALID;

	handle_t *h = handle_get(task, (int)(int64_t)arg0);
	if (h == NULL || h->type != HANDLE_FILE)
		return SYSCALL_RESULT_INVALID;

	// The thread's bounce block (params header only; the data area is idle
	// here). Seek sits on readline's fast path now — probe + surplus
	// seek-back per line — so it sheds its per-call kmalloc/kfree too.
	file_io_params_t *fp = syscall_io_scratch(NULL);
	if (fp == NULL)
		return SYSCALL_RESULT_INVALID;

	fp->file = (vfs_file_t *)h->object;
	fp->offset = (long)(int64_t)arg1;
	fp->whence = (int)(int64_t)arg2;
	fp->result = -1;

	// A FAT seek can walk the cluster chain — disk I/O, kernel context.
	call_in_kernel_context(file_do_seek, fp);

	long pos = fp->result;

	if (pos < 0)
		return SYSCALL_RESULT_INVALID;   // bad whence, or the seek itself failed
	return (uint64_t)pos;
}

// ── map / unmap ──────────────────────────────────────────────────────────────
// THE heap primitive — the ratified no-brk design (DEBTS): the kernel hands
// out whole demand-paged anonymous regions; userland's malloc carves them up.
// There is deliberately no brk/sbrk and never will be: regions are
// independent, so memory can be given back from the MIDDLE of the heap —
// the structural fix for brk's only-shrink-from-the-end flaw.

// map(len) — allocate a fresh anonymous region of at least `len` bytes
// (rounded up to whole pages), demand-paged and GUARANTEED ZEROED (the
// allocator zeroes at its choke point; the demand pager hands those pages
// straight over). Returns the region's base address, or a sentinel.
//
// This is almost embarrassingly thin, and that's the design: creating a
// mapping is just RECORDING INTENT (one VMA node). No pages move until the
// task actually touches memory — first touch faults, the demand pager
// resolves it, page by page, only for pages actually used.
static uint64_t syscall_map(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5;

	size_t length = (size_t)arg0;

	core_local_storage_t *cls = get_core_local_storage();
	task_t *task = cls ? cls->task : NULL;
	if (task == NULL || length == 0)
		return SYSCALL_RESULT_INVALID;

	size_t rounded = (length + PAGE_SIZE - 1) & ~(size_t)(PAGE_SIZE - 1);

	// The heap VA allocator is a bump pointer: heapEnd starts at
	// TASK_HEAP_START and only ever advances. Freed regions' VAs are never
	// reused (v1) — with a 47-bit heap range, address space is the cheapest
	// resource this kernel owns, and never-reuse means a stale pointer into
	// an unmapped region ALWAYS faults instead of aliasing a new region.
	// (A use-after-unmap tripwire for free — same philosophy as the HHDM.)
	uintptr_t base = task->heapEnd;
	if (base + rounded >= TASK_HEAP_END || base + rounded < base)
		return SYSCALL_RESULT_INVALID;   // out of heap VA (a 47-bit feat)

	vma_t *vma = vma_create(base, base + rounded, PROT_READ | PROT_WRITE,
	                        MAP_PRIVATE | MAP_ANONYMOUS, NULL, 0);
	if (vma == NULL)
		return SYSCALL_RESULT_INVALID;
	vma_add(task, vma);

	// Advance past the region PLUS one permanently-unmapped guard page:
	// running off the end of a region faults instead of silently scribbling
	// on the next one. malloc overruns announce themselves here.
	task->heapEnd = base + rounded + PAGE_SIZE;

	printd(DEBUG_SYSCALL, "map: task %s: %lu bytes at 0x%016lx\n",
	       task->exename, rounded, base);
	return (uint64_t)base;
}

// unmap(base) — release an ENTIRE region previously returned by map(). Whole
// regions only, by design: partial unmap invites split-VMA bookkeeping for a
// need malloc doesn't have (it gives back the regions it took). Frees every
// page the task actually touched (untouched pages were never allocated —
// that's demand paging's other half) and drops the VMA.
static uint64_t syscall_unmap(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5;

	uintptr_t base = (uintptr_t)arg0;

	core_local_storage_t *cls = get_core_local_storage();
	task_t *task = cls ? cls->task : NULL;
	if (task == NULL || task->mmaps == NULL)
		return SYSCALL_RESULT_INVALID;

	// Find the region by EXACT base — and only accept regions map() made
	// (anonymous, heap-range). Without this gate, userland could unmap its
	// own code segment and the next instruction fetch becomes a kernel
	// panic; a bad handle should hurt the caller, not the kernel.
	dlist_node_t *node = task->mmaps->head;
	vma_t *vma = NULL;
	while (node != NULL)
	{
		vma_t *v = (vma_t *)node->data;
		if (v != NULL && v->start == base)
		{
			vma = v;
			break;
		}
		node = node->next;
	}
	if (vma == NULL || !(vma->flags & MAP_ANONYMOUS) || base < TASK_HEAP_START)
		return SYSCALL_RESULT_INVALID;

	// Give back every page that was actually faulted in. paging_unmap_page
	// invlpg's locally; pages never touched have no PTE and nothing to free.
	for (uintptr_t va = vma->start; va < vma->end; va += PAGE_SIZE)
	{
		uintptr_t phys = paging_walk_paging_table((pt_entry_t *)task->pml4v, va);
		if (phys != 0 && phys != 0xbadbadba)
		{
			paging_unmap_page((pt_entry_t *)task->pml4v, va);
			// The allocator choke point: also HHDM-unmaps + TLB-shoots the
			// kernel alias, per the lazy-HHDM rules.
			free_memory(phys);
		}
	}

	printd(DEBUG_SYSCALL, "unmap: task %s: region 0x%016lx-0x%016lx released\n",
	       task->exename, vma->start, vma->end);

	dlist_remove(task->mmaps, node);
	vma_destroy(vma);
	return 0;
}

// ── getcwd / chdir ───────────────────────────────────────────────────────────

// getcwd(buf, len) — copy the task's current working directory (a canonical
// absolute path — chdir guarantees it) into the user buffer. Returns the
// path's length, or a sentinel if the buffer is too small. The trivial half
// of the pair: the string is already sitting in the task struct.
static uint64_t syscall_getcwd(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg2; (void)arg3; (void)arg4; (void)arg5;

	core_local_storage_t *cls = get_core_local_storage();
	task_t *task = cls ? cls->task : NULL;
	if (task == NULL || task->cwd == NULL)
		return SYSCALL_RESULT_INVALID;

	size_t len = 0;
	while (task->cwd[len] != '\0')
		len++;
	if ((size_t)arg1 < len + 1)
		return SYSCALL_RESULT_INVALID;   // buffer too small (NUL included)

	if (!copy_to_user_buffer((void *)arg0, task->cwd, len + 1))
		return SYSCALL_RESULT_BAD_USER_DATA;
	return (uint64_t)len;
}

// chdir's kernel-context half: prove the target EXISTS and IS a directory by
// opening it through the real filesystem, then close it again. Existence
// checking at change time is the entire advantage of kernel-owned cwd over
// a writable environment string — a cwd can never hold garbage.
typedef struct {
	char path[TASK_MAX_PATH_LEN];  // FULL canonical path — this becomes cwd
	vfs_filesystem_t *fs;          // mount-resolved from path (task context)
	const char *fs_tail;           // fs-local remainder, points into path (or
	                               // at a static "/") — transient, never freed
	vfs_directory_t *dir;
	volatile long result;
} chdir_params_t;

static void chdir_do(void *arg)
{
	chdir_params_t *p = (chdir_params_t *)arg;

	if (p->fs->dops == NULL || p->fs->dops->open == NULL)
	{
		p->result = -1;
		return;
	}
	p->result = p->fs->dops->open(&p->dir, p->fs_tail, p->fs);
	if (p->result == 0 && p->dir != NULL)
		p->fs->dops->close(p->dir);   // frees what open allocated
}

// chdir(path) — resolve against the current cwd, canonicalize, validate,
// store. After this returns 0, every relative path in every syscall means
// the new place, and the string getcwd hands back is already clean ("cd
// ../../.." from /bin is stored as "/", not as a growing trail of dots).
static uint64_t syscall_chdir(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5;

	core_local_storage_t *cls = get_core_local_storage();
	task_t *task = cls ? cls->task : NULL;
	if (task == NULL || task->cwd == NULL)
		return SYSCALL_RESULT_INVALID;

	char raw[TASK_MAX_PATH_LEN];
	if (!copy_user_string((const char *)arg0, raw, sizeof(raw)))
		return SYSCALL_RESULT_BAD_USER_DATA;

	chdir_params_t *p = kmalloc(sizeof(*p));
	if (p == NULL)
		return SYSCALL_RESULT_INVALID;

	if (!resolve_user_path(task, raw, p->path, sizeof(p->path)))
	{
		kfree(p);
		return SYSCALL_RESULT_BAD_USER_DATA;
	}

	// Route to the mounted filesystem (cwd may cross into "/fat", "/ext2"…).
	// The FULL canonical path is what cwd stores; the fs only validates its
	// own tail. A bare mount prefix ("cd /fat") yields tail "/" — the fs root
	// — which always exists, so mount prefixes are always enterable.
	p->fs_tail = NULL;
	p->fs = vfs_resolve_mount(p->path, &p->fs_tail);
	if (p->fs == NULL)
	{
		kfree(p);
		return SYSCALL_RESULT_INVALID;   // nothing mounted yet
	}

	p->dir = NULL;
	p->result = -1;
	// The existence check walks the directory tree — disk I/O, kernel context.
	call_in_kernel_context(chdir_do, p);

	if (p->result != 0)
	{
		printd(DEBUG_SYSCALL, "chdir: task %s: '%s' is not a directory\n",
		       task->exename, p->path);
		kfree(p);
		return SYSCALL_RESULT_INVALID;   // no such directory — cwd unchanged
	}

	// Committed: the canonical path becomes the task's "here".
	// (task->cwd is a PAGE_SIZE kmalloc from task_create; TASK_MAX_PATH_LEN
	// fits with room to spare.)
	memcpy(task->cwd, p->path, sizeof(p->path));
	printd(DEBUG_SYSCALL, "chdir: task %s: cwd = '%s'\n", task->exename, task->cwd);
	kfree(p);
	return 0;
}

// unlink's kernel-context half: one fops->rm. Deleting a file walks the
// directory and rewrites the FAT, so it is disk I/O like every other path
// operation here.
typedef struct {
	char path[TASK_MAX_PATH_LEN];  // full canonical path (kept for logging)
	vfs_filesystem_t *fs;          // mount-resolved in task context
	const char *fs_tail;           // fs-local remainder; points into path or
	                               // at a static "/" — transient, never freed
	volatile long result;
} unlink_params_t;

static void unlink_do(void *arg)
{
	unlink_params_t *p = (unlink_params_t *)arg;
	// The NULL check is the read-only answer: ext2 never installs rm, and
	// dispatching through a NULL fop would execute mapped page zero rather
	// than fail (fat_glue.c's disk_write learned this the hard way).
	p->result = (p->fs->fops != NULL && p->fs->fops->rm != NULL)
	                ? p->fs->fops->rm(p->fs_tail, p->fs)
	                : -1;
}

static uint64_t syscall_unlink(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5;

	core_local_storage_t *cls = get_core_local_storage();
	task_t *task = cls ? cls->task : NULL;
	if (task == NULL)
		return SYSCALL_RESULT_INVALID;

	char raw[TASK_MAX_PATH_LEN];
	if (!copy_user_string((const char *)arg0, raw, sizeof(raw)))
		return SYSCALL_RESULT_BAD_USER_DATA;

	unlink_params_t *p = kmalloc(sizeof(*p));
	if (p == NULL)
		return SYSCALL_RESULT_INVALID;

	// Relative paths resolve against the task's cwd, same as open — `rm
	// notes.txt` has to mean the same file `cat notes.txt` just printed.
	if (!resolve_user_path(task, raw, p->path, sizeof(p->path)))
	{
		kfree(p);
		return SYSCALL_RESULT_BAD_USER_DATA;
	}

	p->fs_tail = NULL;
	p->fs = vfs_resolve_mount(p->path, &p->fs_tail);
	if (p->fs == NULL)
	{
		kfree(p);
		return SYSCALL_RESULT_INVALID;   // nothing mounted yet
	}

	// Refuse to delete a mount point itself. "/home" resolves to tail "/",
	// which is the filesystem's ROOT — handing that to f_unlink is asking a
	// driver to delete the volume it lives on. `rm /home` is a typo, always.
	if (p->fs_tail == NULL || p->fs_tail[0] == '\0' ||
	    (p->fs_tail[0] == '/' && p->fs_tail[1] == '\0'))
	{
		kfree(p);
		return SYSCALL_RESULT_INVALID;
	}

	p->result = -1;
	call_in_kernel_context(unlink_do, p);

	if (p->result != 0)
	{
		printd(DEBUG_SYSCALL, "unlink: task %s: could not delete '%s' (read-only fs, missing file, or non-empty directory)\n",
		       task->exename, p->path);
		kfree(p);
		return SYSCALL_RESULT_INVALID;
	}

	printd(DEBUG_SYSCALL, "unlink: task %s: deleted '%s'\n", task->exename, p->path);
	kfree(p);
	return 0;
}

// mkdir's kernel-context half: one dops->mkdir. Creating a directory
// allocates a cluster and writes two directory entries, so it is disk I/O
// like every other path operation here. (mkdir lives in dops rather than
// fops for the same reason stat does: directories are directory-entry
// vocabulary.)
typedef struct {
	char path[TASK_MAX_PATH_LEN];  // full canonical path (kept for logging)
	vfs_filesystem_t *fs;          // mount-resolved in task context
	const char *fs_tail;           // fs-local remainder; points into path or
	                               // at a static "/" — transient, never freed
	volatile long result;
} mkdir_params_t;

static void mkdir_do(void *arg)
{
	mkdir_params_t *p = (mkdir_params_t *)arg;
	// The NULL check is the read-only answer: ext2 never installs mkdir, and
	// dispatching through a NULL dop would execute mapped page zero rather
	// than fail (fat_glue.c's disk_write learned this the hard way).
	// The cast: dops->mkdir takes char* because fat_mkdir copies-then-mangles
	// its own scratch buffer; the tail itself is never written through.
	p->result = (p->fs->dops != NULL && p->fs->dops->mkdir != NULL)
	                ? p->fs->dops->mkdir((char *)p->fs_tail, p->fs)
	                : -1;
}

static uint64_t syscall_mkdir(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5;

	core_local_storage_t *cls = get_core_local_storage();
	task_t *task = cls ? cls->task : NULL;
	if (task == NULL)
		return SYSCALL_RESULT_INVALID;

	char raw[TASK_MAX_PATH_LEN];
	if (!copy_user_string((const char *)arg0, raw, sizeof(raw)))
		return SYSCALL_RESULT_BAD_USER_DATA;

	mkdir_params_t *p = kmalloc(sizeof(*p));
	if (p == NULL)
		return SYSCALL_RESULT_INVALID;

	// Relative paths resolve against the task's cwd, same as open — `mkdir
	// notes` has to make the directory `cd notes` will then enter.
	if (!resolve_user_path(task, raw, p->path, sizeof(p->path)))
	{
		kfree(p);
		return SYSCALL_RESULT_BAD_USER_DATA;
	}

	p->fs_tail = NULL;
	p->fs = vfs_resolve_mount(p->path, &p->fs_tail);
	if (p->fs == NULL)
	{
		kfree(p);
		return SYSCALL_RESULT_INVALID;   // nothing mounted yet
	}

	// Refuse to create a mount point itself. "/home" resolves to tail "/",
	// which is the filesystem's ROOT — it already exists by definition, and
	// handing "/" to f_mkdir is asking a driver to create the volume it
	// lives on. `mkdir /fat` is a typo, always.
	if (p->fs_tail == NULL || p->fs_tail[0] == '\0' ||
	    (p->fs_tail[0] == '/' && p->fs_tail[1] == '\0'))
	{
		kfree(p);
		return SYSCALL_RESULT_INVALID;
	}

	p->result = -1;
	call_in_kernel_context(mkdir_do, p);

	if (p->result != 0)
	{
		printd(DEBUG_SYSCALL, "mkdir: task %s: could not create '%s' (read-only fs, missing parent, or name taken)\n",
		       task->exename, p->path);
		kfree(p);
		return SYSCALL_RESULT_INVALID;
	}

	printd(DEBUG_SYSCALL, "mkdir: task %s: created '%s'\n", task->exename, p->path);
	kfree(p);
	return 0;
}

// rename's kernel-context half: one fops->rename. Both paths are already
// canonical and mount-resolved, and both were proven to belong to the SAME
// filesystem in task context — the driver below is handed two fs-local
// tails and never has to wonder whether they're on the same volume.
typedef struct {
	char oldpath[TASK_MAX_PATH_LEN];   // full canonical paths (kept for logging)
	char newpath[TASK_MAX_PATH_LEN];
	vfs_filesystem_t *fs;              // the one filesystem BOTH paths live on
	const char *old_tail;              // fs-local remainders; point into the
	const char *new_tail;              //   buffers above — transient, never freed
	volatile long result;
} rename_params_t;

static void rename_do(void *arg)
{
	rename_params_t *p = (rename_params_t *)arg;
	// The NULL check is the read-only answer: a filesystem with no write
	// path leaves this slot NULL, and dispatching through it would execute
	// mapped page zero rather than fail (fat_glue.c's disk_write, again).
	p->result = (p->fs->fops != NULL && p->fs->fops->rename != NULL)
	                ? p->fs->fops->rename(p->old_tail, p->new_tail, p->fs)
	                : -1;
}

// rename(oldpath, newpath) — see the contract over SYSCALL_RENAME in
// syscall_numbers.h. This half owns exactly three jobs: get both strings
// safely out of user space, resolve both against the task's cwd and the
// mount table, and refuse the two cases no filesystem driver should ever be
// asked about (a cross-mount rename, and a mount point as either end).
static uint64_t syscall_rename(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg2; (void)arg3; (void)arg4; (void)arg5;

	core_local_storage_t *cls = get_core_local_storage();
	task_t *task = cls ? cls->task : NULL;
	if (task == NULL)
		return SYSCALL_RESULT_INVALID;

	char raw_old[TASK_MAX_PATH_LEN];
	char raw_new[TASK_MAX_PATH_LEN];
	if (!copy_user_string((const char *)arg0, raw_old, sizeof(raw_old)))
		return SYSCALL_RESULT_BAD_USER_DATA;
	if (!copy_user_string((const char *)arg1, raw_new, sizeof(raw_new)))
		return SYSCALL_RESULT_BAD_USER_DATA;

	rename_params_t *p = kmalloc(sizeof(*p));
	if (p == NULL)
		return SYSCALL_RESULT_INVALID;

	// Relative paths resolve against the task's cwd, same as open and unlink
	// — `mv notes.txt old.txt` has to mean the files `ls` just listed.
	if (!resolve_user_path(task, raw_old, p->oldpath, sizeof(p->oldpath)) ||
	    !resolve_user_path(task, raw_new, p->newpath, sizeof(p->newpath)))
	{
		kfree(p);
		return SYSCALL_RESULT_BAD_USER_DATA;
	}

	p->old_tail = NULL;
	p->new_tail = NULL;
	p->fs = vfs_resolve_mount(p->oldpath, &p->old_tail);
	vfs_filesystem_t *new_fs = vfs_resolve_mount(p->newpath, &p->new_tail);
	if (p->fs == NULL || new_fs == NULL)
	{
		kfree(p);
		return SYSCALL_RESULT_INVALID;   // nothing mounted yet
	}

	// THE CROSS-MOUNT REFUSAL. Two paths on two filesystems have nothing in
	// common but the namespace they're spelled in; there is no directory
	// entry to move, only bytes to copy. Refusing here (rather than letting
	// a driver discover it) is what keeps every fops->rename implementation
	// free of the question. `mv` across mounts is copy-then-unlink, in
	// userland, where a half-finished copy can be reasoned about.
	if (new_fs != p->fs)
	{
		printd(DEBUG_SYSCALL, "rename: task %s: '%s' and '%s' are on different filesystems — refused (copy, don't rename)\n",
		       task->exename, p->oldpath, p->newpath);
		kfree(p);
		return SYSCALL_RESULT_INVALID;
	}

	// Refuse a mount point as either end, for the reason syscall_unlink
	// gives: "/home" resolves to the tail "/", which is the filesystem's
	// ROOT. Handing that to a driver is asking it to rename the volume it
	// lives on out of existence.
	if (p->old_tail == NULL || p->old_tail[0] == '\0' ||
	    (p->old_tail[0] == '/' && p->old_tail[1] == '\0') ||
	    p->new_tail == NULL || p->new_tail[0] == '\0' ||
	    (p->new_tail[0] == '/' && p->new_tail[1] == '\0'))
	{
		kfree(p);
		return SYSCALL_RESULT_INVALID;
	}

	p->result = -1;
	call_in_kernel_context(rename_do, p);

	if (p->result != 0)
	{
		printd(DEBUG_SYSCALL, "rename: task %s: could not rename '%s' -> '%s' (read-only fs, missing source, open handle, or a refused replacement)\n",
		       task->exename, p->oldpath, p->newpath);
		kfree(p);
		return SYSCALL_RESULT_INVALID;
	}

	printd(DEBUG_SYSCALL, "rename: task %s: '%s' -> '%s'\n",
	       task->exename, p->oldpath, p->newpath);
	kfree(p);
	return 0;
}

// stat's kernel-context half: one dops->stat, into the HHDM params block.
typedef struct {
	char path[TASK_MAX_PATH_LEN];  // full canonical path (kept for logging)
	vfs_filesystem_t *fs;          // mount-resolved in task context
	const char *fs_tail;           // fs-local remainder; points into path or
	                               // at a static "/" — transient, never freed
	os64_dirent_t entry;           // out, when result == 0
	volatile long result;
} stat_params_t;

static void stat_do(void *arg)
{
	stat_params_t *p = (stat_params_t *)arg;
	p->result = (p->fs->dops != NULL && p->fs->dops->stat != NULL)
	                ? p->fs->dops->stat(p->fs_tail, &p->entry, p->fs) : -1;
}

// stat(path, entry_out) — fill one os64_dirent_t for the object at `path`,
// file or directory: the answer to "what is this one name?" without opening
// it. Same struct readdir delivers — stat is readdir for exactly one name —
// so callers speak ONE vocabulary for "an entry" (this is 1971's oldest
// surviving syscall idea, minus POSIX's stat-vs-dirent split). Returns 0
// with *entry_out filled, or a SYSCALL_RESULT_* sentinel / negative on
// absence. ls uses this to tell a file argument from a directory argument.
static uint64_t syscall_stat(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg2; (void)arg3; (void)arg4; (void)arg5;

	core_local_storage_t *cls = get_core_local_storage();
	task_t *task = cls ? cls->task : NULL;
	if (task == NULL)
		return SYSCALL_RESULT_INVALID;

	char raw[TASK_MAX_PATH_LEN];
	if (!copy_user_string((const char *)arg0, raw, sizeof(raw)))
		return SYSCALL_RESULT_BAD_USER_DATA;

	stat_params_t *p = kmalloc(sizeof(*p));
	if (p == NULL)
		return SYSCALL_RESULT_INVALID;

	if (!resolve_user_path(task, raw, p->path, sizeof(p->path)))
	{
		kfree(p);
		return SYSCALL_RESULT_BAD_USER_DATA;
	}

	p->fs_tail = NULL;
	p->fs = vfs_resolve_mount(p->path, &p->fs_tail);
	if (p->fs == NULL)
	{
		kfree(p);
		return SYSCALL_RESULT_INVALID;   // nothing mounted yet
	}

	p->result = -1;
	// Path resolution walks the directory tree — disk I/O, kernel context.
	call_in_kernel_context(stat_do, p);

	if (p->result != 0)
	{
		printd(DEBUG_SYSCALL, "stat: task %s: '%s' — no such path\n",
		       task->exename, p->path);
		kfree(p);
		return SYSCALL_RESULT_INVALID;
	}

	if (!copy_to_user_buffer((void *)arg1, &p->entry, sizeof(p->entry)))
	{
		kfree(p);
		return SYSCALL_RESULT_BAD_USER_DATA;
	}

	printd(DEBUG_SYSCALL, "stat: task %s: '%s' -> %s, size %lu\n",
	       task->exename, p->path,
	       (p->entry.flags & OS64_DE_DIR) ? "directory" : "file",
	       p->entry.size);
	kfree(p);
	return 0;
}

// Marshal a user argv (NULL-terminated array of user string pointers) into
// kernel space: fills kargv[] (<= SPAWN_MAX_ARGS, NULL-terminated) with
// pointers into strbuf, each string copied from user space. Returns argc, or
// -1 on a bad user pointer. NULL user_argv means "no args" (argc 0).
// Count the entries in a user argv[] without touching the strings — reads only
// the pointer array, stopping at the NULL terminator or SPAWN_MAX_ARGS.
// Returns the count, or -1 on a bad user pointer.
//
// Exists so the spawn block can be sized to the ACTUAL argument count instead
// of the maximum. With SPAWN_MAX_ARGS at 512 and TASK_MAX_PATH_LEN at 256, a
// fixed worst-case block would be 128KB kmalloc'd AND ZEROED on every spawn —
// on `pwd`, on every prompt. Measured first, an ordinary command's block is a
// few hundred bytes: cheaper than the 4KB the fixed 32-argument version cost.
static int count_user_argv(char *const *user_argv)
{
	if (user_argv == NULL)
		return 0;

	for (int argc = 0; argc < SPAWN_MAX_ARGS; argc++)
	{
		char *uptr = NULL;
		if (!copy_user_buffer(&user_argv[argc], &uptr, sizeof(char *)))
			return -1;
		if (uptr == NULL)
			return argc;
	}
	return SPAWN_MAX_ARGS;
}

static int marshal_user_argv(char *const *user_argv, char *kargv[],
                             char *strbuf, size_t strbuf_len, int max_args)
{
	if (user_argv == NULL)
	{
		kargv[0] = NULL;
		return 0;
	}

	size_t used = 0;
	int argc = 0;
	// Bounded by max_args, NOT by SPAWN_MAX_ARGS: the caller sized kargv[] from
	// a COUNT PASS over this same user memory, and userland can modify its own
	// argv between the two passes (another thread, or a deliberately hostile
	// program). Trusting the second walk to agree with the first would let a
	// racing user drive writes past the end of the allocated pointer array —
	// a kernel heap overflow from ring 3. The count is the contract; this walk
	// stops there and the string buffer's own `avail` check covers the rest.
	for (argc = 0; argc < max_args; argc++)
	{
		// Read user_argv[argc] — a user-space char* — into the kernel.
		char *uptr = NULL;
		if (!copy_user_buffer(&user_argv[argc], &uptr, sizeof(char *)))
			return -1;
		if (uptr == NULL)
			break;   // NULL terminator: end of argv

		char *dst = strbuf + used;
		size_t avail = strbuf_len - used;
		if (avail < 2)
			return -1;   // out of scratch space
		size_t cap = avail < TASK_MAX_PATH_LEN ? avail : TASK_MAX_PATH_LEN;
		if (!copy_user_string((const char *)uptr, dst, cap))
			return -1;

		kargv[argc] = dst;
		// Advance past the copied (NUL-terminated) string.
		size_t slen = 0;
		while (dst[slen] != '\0')
			slen++;
		used += slen + 1;
	}
	kargv[argc] = NULL;
	return argc;
}

// All the state task_create() needs, in ONE kmalloc'd (HHDM) block so it is
// reachable from BOTH the syscall CR3 (to fill in) AND kKernelPML4 (where the
// helper below runs). Nothing here may live on the syscall's task-local kernel
// stack — kKernelPML4 doesn't map it. argv[] points into argvstrs[], both in
// this block.
typedef struct {
	char   path[TASK_MAX_PATH_LEN];
	int    argc;
	// Both point into `tail` below, which is sized at allocation time to the
	// ACTUAL argument count (see count_user_argv). These were fixed arrays
	// until 2026-08-13; at the 512-argument ceiling that shell globbing needs,
	// a fixed worst case would have been ~135KB allocated and zeroed on every
	// single spawn. Sized to fit, `pwd` costs a few hundred bytes.
	char **argv;
	char  *argvstrs;
	task_t *parent;
	// Redirection for the child's slots 0/1/2, RESOLVED from the parent's
	// handle numbers before we leave the caller's context (handle tables are
	// per-task, and in there we still know who the caller is). HANDLE_NONE
	// means "leave the child's default" — i.e. the console.
	handle_type_t redirType[3];
	void  *redirObject[3];
	// OS64_SPAWN_SET_TTY (PTY.md): the pty slave to seat the child on, or
	// NULL for plain inheritance. Resolved from the master handle in the
	// caller's context, like the redirections — same reasoning, same seam.
	tty_t *ttySlave;
	bool   background;                    // OS64_SPAWN_BACKGROUND (`&`)
	volatile long result;                 // child pid, or -1 on failure
	// [ (argc+1) pointer slots ][ argc * TASK_MAX_PATH_LEN bytes of strings ].
	// One allocation, so the whole thing stays HHDM-contiguous and reachable
	// from kKernelPML4 exactly as the comment above requires.
	char   tail[];
} spawn_params_t;

// Runs under kKernelPML4 (via call_in_kernel_context), because task_create
// does disk I/O — the ELF load touches NVMe/AHCI DMA regions that are mapped
// in the kernel tables but NOT in a user task's CR3 (which is why calling
// task_create directly from the syscall faulted in nvme_submit_command). Same
// discipline the demand-pager uses for kernel_read_file.
static void spawn_do_create(void *arg)
{
	spawn_params_t *p = (spawn_params_t *)arg;
	task_t *child = task_create(p->path, p->argc, p->argv, p->parent,
	                            false, THREAD_NO_AFFINITY);
	if (child == NULL)
	{
		p->result = -1;
		return;
	}

	// Marked BEFORE the child is submitted to the scheduler, for the same
	// reason redirection is applied below: it must not get one instruction of
	// CPU in the wrong state. A background job that ran even briefly as a
	// foreground one could steal a keystroke, and that race would be
	// spectacularly hard to see.
	//
	// AND IT IS INHERITED. The debt booked at this line said "the day husk
	// runs scripts, this is where the ruling lands" — husk runs scripts as of
	// this branch, so it lands now (Codex review, 2026-08-23): `myscript &`
	// ran its commands from a background interpreter, and every command
	// INSIDE the script was minted foreground, so a `cat` in a backgrounded
	// script rejoined the keyboard queue and took keystrokes the interactive
	// husk was owed — the exact hole `&` closes, reopened one generation down.
	//
	// Inheritance is also what the two mechanisms real shells use both do,
	// checked rather than assumed (bash + dash under a pty, 2026-08-23):
	// with job control OFF, POSIX assigns an asynchronous list's stdin to
	// /dev/null, and a background job's CHILD and GRANDCHILD both showed
	// fd 0 = /dev/null while a foreground child showed the terminal; with
	// job control ON, the background job gets its own process group and
	// reading the tty earns SIGTTIN — and a child inherits the process group.
	// Both are inherited BY CONSTRUCTION, because both are properties a fork
	// carries. os64's flag is a task field, so it has to say so out loud.
	//
	// Once background, always background, for the whole descendant tree ON
	// THE SAME TERMINAL. The boundary matters, and the first version of this
	// line (2026-08-23, a few hours old) missed it: `gterm &` made gterm a
	// background job — correct — and the husk it seated on a brand-new pty
	// inherited the flag, so every read of ITS OWN terminal came back EOF and
	// nothing typed into the window reached it. Chris found it on the P5 the
	// same morning. In Unix terms a job is background WITH RESPECT TO A TTY
	// (that is what a process group is — per-terminal, not per-tree), and a
	// child seated on a fresh terminal is that terminal's session leader,
	// foreground by definition there. `xterm &` is the oldest spelling of
	// this: the same `&` that keeps a `cat &` off the keyboard hands the
	// xterm's shell a keyboard of its own. So inheritance stops at a seat.
	// (The caller's OWN `&` is still honoured across one — that is an
	// explicit ask, not an inheritance.)
	child->backgroundJob = p->background ||
	                      (p->parent != NULL && p->parent->backgroundJob &&
	                       p->ttySlave == NULL);

	// Seat on a pty slave (PTY.md), BEFORE submission like everything else
	// here: the child must never run an instruction on the wrong terminal.
	// tty_seat_shell is the SAME seat the knock-summon gives a husk on a VT
	// — controlling shell, terminal of record, foreground, lights on — which
	// is deliberate: the seated child is the session, that is what seating
	// means, so seat a shell. The seat bookkeeping swaps the inherited
	// terminal's pty reference (taken in task_create) for the slave's.
	if (p->ttySlave != NULL)
	{
		tty_pty_unref((tty_t *)child->tty);
		tty_seat_shell(p->ttySlave, child);
		tty_pty_ref(p->ttySlave);
	}

	// Apply redirection BEFORE the child is submitted to the scheduler — the
	// child must never get a single instruction of CPU with the wrong handles
	// in slots 0/1/2. It is born already reading from and writing to the right
	// places, and it will never know it was redirected. That is the whole trick
	// behind `a | b`: neither program contains one line of pipe-awareness.
	for (int slot = 0; slot < 3; slot++)
	{
		if (p->redirType[slot] == HANDLE_NONE)
			continue;   // keep the child's default (console)

		// The child gets its OWN reference on the pipe end. Two tasks now hold
		// this end; both must close it before the refcount reaches zero and the
		// EOF/EPIPE fires. (This ref is why the shell closing its copy does not
		// yank the pipe out from under the child.)
		if (p->redirType[slot] == HANDLE_PIPE_READ)
			pipe_ref_read_end((pipe_t *)p->redirObject[slot]);
		else if (p->redirType[slot] == HANDLE_PIPE_WRITE)
			pipe_ref_write_end((pipe_t *)p->redirObject[slot]);
		// Files share by the same rule (handleRefCount, vfs.h): the child's
		// slot is a second reference on ONE open file, and only the last
		// close runs the VFS close. This is what makes `upper < file` safe:
		// the shell opens, spawns, and immediately closes its copy — the
		// child's handle survives because the refcount says so. (Note the
		// child inherits the file POSITION too — shared FIL, dup semantics —
		// which for a just-opened redirect file is position 0, as intended.)
		else if (p->redirType[slot] == HANDLE_FILE)
			__sync_add_and_fetch(&((vfs_file_t *)p->redirObject[slot])->handleRefCount, 1);

		handle_install(child, slot, p->redirType[slot], p->redirObject[slot]);
	}

	scheduler_submit_new_task(child);
	p->result = (long)child->taskID;
}

// spawn(path, argv, in, out, err) — launch `path` as a child of the calling
// task and return its pid, or a SYSCALL_RESULT_* sentinel. Non-blocking: the
// child is submitted to the scheduler and runs concurrently; the caller reaps
// it with wait(). The child inherits the caller's environment (via task_create's
// parent).
//
// in/out/err are the CALLER'S handle numbers to install as the child's 0/1/2,
// or -1 to leave that stream on the console. This is how a shell builds a
// pipeline: spawn(a, ..., -1, pipeW, -1) and spawn(b, ..., pipeR, -1, -1). We
// deliberately do NOT blanket-inherit the parent's whole handle table the way
// Unix does — a child gets the console plus exactly what was asked for, and can
// never accidentally hold some unrelated pipe end open (a classic pipeline hang).
static uint64_t syscall_spawn(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	const char *user_path = (const char *)arg0;
	char *const *user_argv = (char *const *)arg1;

	// arg5 = flags (OS64_SPAWN_*). Zero is the everyday spawn, so every caller
	// written before this argument existed keeps working untouched — arg5 was
	// previously ignored, and "ignored" and "zero means normal" agree.
	// Unknown bits are REFUSED at the boundary rather than ignored: a flag the
	// kernel silently drops is a request that appeared to succeed and didn't,
	// the same reasoning that rejects unknown open() modes.
	uint64_t flags = arg5;
	// The LOW 32 bits are flags; the HIGH 32 carry SET_TTY's master handle
	// (syscall_numbers.h says why one register carries both). Only unknown
	// LOW bits are a caller bug — the high half is data, judged below.
	if ((flags & 0xFFFFFFFFull &
	     ~(uint64_t)(OS64_SPAWN_BACKGROUND | OS64_SPAWN_SET_TTY)) != 0)
		return SYSCALL_RESULT_BAD_USER_DATA;

	// Size the block to the arguments actually present. The count pass reads
	// only the user's pointer array; the strings are copied once, below.
	int argcHint = count_user_argv(user_argv);
	if (argcHint < 0)
		return SYSCALL_RESULT_BAD_USER_DATA;
	size_t argvPtrBytes = (size_t)(argcHint + 1) * sizeof(char *);
	size_t argvStrBytes = (size_t)argcHint * TASK_MAX_PATH_LEN;

	spawn_params_t *p = kmalloc(sizeof(*p) + argvPtrBytes + argvStrBytes);
	if (p == NULL)
		return SYSCALL_RESULT_INVALID;
	p->argv     = (char **)p->tail;
	p->argvstrs = p->tail + argvPtrBytes;
	p->background = (flags & OS64_SPAWN_BACKGROUND) != 0;

	// Marshal path + argv from user space into the HHDM block (runs on the
	// caller's CR3, which maps both the user args and the HHDM).
	if (!copy_user_string(user_path, p->path, sizeof(p->path)))
	{
		kfree(p);
		return SYSCALL_RESULT_BAD_USER_DATA;
	}
	int argc = marshal_user_argv(user_argv, p->argv, p->argvstrs, argvStrBytes, argcHint);
	if (argc < 0)
	{
		kfree(p);
		return SYSCALL_RESULT_BAD_USER_DATA;
	}
	p->argc = argc;

	core_local_storage_t *cls = get_core_local_storage();
	p->parent = cls ? cls->task : NULL;
	p->result = 0;

	// Resolve the program path against the caller's cwd, so `cd /bin` makes
	// a bare `hello` launchable. (The child then INHERITS that cwd — the
	// task_create plumbing for that predates these syscalls by a while.)
	{
		char canon[TASK_MAX_PATH_LEN];
		if (!resolve_user_path(p->parent, p->path, canon, sizeof(canon)))
		{
			kfree(p);
			return SYSCALL_RESULT_BAD_USER_DATA;
		}
		memcpy(p->path, canon, sizeof(canon));
	}

	// Resolve the redirection handles HERE, in the caller's context, where the
	// caller's handle table is the one in scope. spawn_do_create runs under
	// kKernelPML4 and only ever sees the resolved (type, object) pairs.
	int64_t redir[3] = { (int64_t)arg2, (int64_t)arg3, (int64_t)arg4 };
	for (int slot = 0; slot < 3; slot++)
	{
		p->redirType[slot] = HANDLE_NONE;
		p->redirObject[slot] = NULL;

		if (redir[slot] < 0)
			continue;   // -1 = leave this stream on the console

		handle_t *h = (p->parent != NULL) ? handle_get(p->parent, (int)redir[slot]) : NULL;
		if (h == NULL)
		{
			kfree(p);
			return SYSCALL_RESULT_INVALID;   // caller passed a bogus handle
		}

		// Directory handles don't cross the spawn boundary: a child's 0/1/2
		// are byte streams, and a directory isn't one — and unlike files and
		// pipe ends, vfs_directory_t has no refcount, so sharing one would
		// dangle the child's copy the moment the parent closes. Reject here,
		// where the caller gets a clean error instead of a haunted handle.
		// Net handles are refused for the same refcount reason (udp_conn_t
		// has a single owner by design) — a "hand the child my connection"
		// story arrives with a refcount when a real consumer wants it.
		if (h->type == HANDLE_DIR || h->type == HANDLE_NET_UDP ||
		    h->type == HANDLE_NET_TCP || h->type == HANDLE_NET_ICMP)
		{
			kfree(p);
			return SYSCALL_RESULT_INVALID;
		}

		p->redirType[slot] = h->type;
		p->redirObject[slot] = h->object;
	}

	// Resolve the SET_TTY master the same way, in the caller's context, where
	// the caller's handle table is in scope (PTY.md's seat).
	p->ttySlave = NULL;
	if (flags & OS64_SPAWN_SET_TTY)
	{
		int mh = (int)(flags >> OS64_SPAWN_TTY_SHIFT);
		handle_t *h = (p->parent != NULL) ? handle_get(p->parent, mh) : NULL;
		if (h == NULL || h->type != HANDLE_PTY_MASTER)
		{
			kfree(p);
			return SYSCALL_RESULT_INVALID;   // not a master you hold
		}
		p->ttySlave = (tty_t *)h->object;
	}

	// task_create runs under kKernelPML4 so its disk I/O sees the DMA mappings.
	call_in_kernel_context(spawn_do_create, p);

	long r = p->result;
	kfree(p);
	if (r < 0)
		return SYSCALL_RESULT_INVALID;   // bad path / load failure
	return (uint64_t)r;
}

// wait(pid, exit_code_out) — block until a child exits, reap it, return its
// pid. pid > 0 waits for that specific child; pid == 0 waits for the FIRST of
// any child to end (os64's own design, not POSIX). Writes the child's exit
// code to *exit_code_out (if non-NULL). Returns the ended pid, or a
// SYSCALL_RESULT_* sentinel (e.g. no matching child exists). If the child has
// ALREADY exited, returns immediately without sleeping.
static uint64_t syscall_wait(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg2; (void)arg3; (void)arg4; (void)arg5;

	uint64_t targetPid = arg0;
	int *user_code = (int *)arg1;

	core_local_storage_t *cls = get_core_local_storage();
	task_t *parent = cls ? cls->task : NULL;
	if (parent == NULL)
		return SYSCALL_RESULT_INVALID;

	uint64_t exitCode = 0;
	// task_wait returns the ended child's ID, not a pointer — by the time we
	// are back here the corpse is collected and kworker may bury it any tick.
	uint64_t endedPid = task_wait(parent, targetPid, &exitCode);
	if (endedPid == 0)
		return SYSCALL_RESULT_INVALID;   // no such child

	if (user_code != NULL)
	{
		int code = (int)exitCode;
		if (!copy_to_user_buffer(user_code, &code, sizeof(code)))
			return SYSCALL_RESULT_BAD_USER_DATA;
	}
	return endedPid;
}

// reap(exit_code_out) — collect ONE finished child without ever blocking.
// Returns that child's task ID, or 0 for "nobody has died", or a
// SYSCALL_RESULT_* sentinel.
//
// A separate syscall rather than a flag on wait(), on purpose: a "wait" that
// does not wait is a name that lies, and this kernel has opinions about those.
// The three-way answer is readdir's, not Unix's — 0 means the perfectly
// ordinary "nothing to collect right now", not an error, so a shell can poll
// it at every prompt without treating the common case as a failure.
//
// This is what makes `&` clean. A shell that reports `[1]+ 57 Done` has to
// COLLECT that status, and collecting IS reaping — so the report and the
// cleanup are one act, and background jobs never pile up as zombies the way
// os32's did (its kshell forked, skipped the waitpid, and never spoke of the
// child again). The shell buries its own dead; kworker never has to care.
static uint64_t syscall_reap(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5;

	int *user_code = (int *)arg0;

	core_local_storage_t *cls = get_core_local_storage();
	task_t *parent = cls ? cls->task : NULL;
	if (parent == NULL)
		return SYSCALL_RESULT_INVALID;

	uint64_t exitCode = 0;
	// Same ID-not-pointer contract as wait: collecting licenses the burial.
	uint64_t endedPid = task_reap_any_dead(parent, &exitCode);
	if (endedPid == 0)
		return 0;   // no finished children — the ordinary answer, not an error

	if (user_code != NULL)
	{
		int code = (int)exitCode;
		if (!copy_to_user_buffer(user_code, &code, sizeof(code)))
			return SYSCALL_RESULT_BAD_USER_DATA;
	}
	return endedPid;
}

// sleep(ms) — park the calling thread for AT LEAST `ms` milliseconds.
//
// The kernel half of this syscall is OLDER THAN THE SYSCALL: the signal
// struct has carried wake-ticks in sigdata[SIGSLEEP] since os32 (2012), and
// processSignals has both halves of the wake — the deadline sweep, and the
// terminate-outranks-the-nap cancel built for Ctrl+C. This handler is just
// the door that lets ring 3 ask for what the kernel already knew how to do.
//
// Units: the ABI speaks TIME (ms); the kernel speaks ticks. The conversion
// lives here, at the boundary, rounding UP against the ACTIVE rate — so a
// kernel rebuilt at 1ms ticks silently improves every existing binary's
// sleep(1) from 10ms to 1ms, and no tick constant ever calcifies into the
// ABI. "At least ms" is the only honest promise: park latency, another
// task's slice, and the signal sweep all land on top of the deadline.
//
// sleep(0) is the documented free yield — not folklore, not an accident.
//
// Interruption: a pending SIGINT/SIGKILL wakes the sleeper (processSignals'
// nap-cancel) and the loop-top check below dies on the same rail as an
// interrupted console read. Returns 0 always — the only interruption that
// exists today is death, and the dead read no return values. Remaining-time
// semantics deliberately wait for the SIGNALS.md EINTR-vs-restart ruling.
static uint64_t syscall_sleep(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5;

	uint64_t ms = arg0;

	core_local_storage_t *cls = get_core_local_storage();
	thread_t *self = cls ? cls->currentThread : NULL;
	if (self == NULL)
		return SYSCALL_RESULT_INVALID;

	if (ms == 0)
	{
		// No time requested: the CPU goes back, nothing is parked. Same
		// trigger as syscall_yield — two honest doors onto one scheduler.
		scheduler_trigger(NULL);
		return 0;
	}

	// ms → ticks at the active rate, rounding UP; ms >= 1 guarantees at
	// least one tick without a separate floor. The clamp keeps absurd
	// requests from overflowing the multiply — capped, that's roughly
	// 2.9 billion years at 100 ticks/second, which is close enough to
	// "forever" that nobody will file a bug about the early wake.
	uint64_t ticks;
	if (ms > (UINT64_MAX - 999) / TICKS_PER_SECOND)
		ticks = UINT64_MAX / 2;
	else
		ticks = (ms * TICKS_PER_SECOND + 999) / 1000;

	uint64_t wakeTick = kTicksSinceStart + ticks;

	// Level-triggered like every park in this kernel: check, sleep, re-check.
	// processSignals wakes us for exactly two reasons — the deadline arrived,
	// or a terminate is pending — and the loop top distinguishes them.
	for (;;)
	{
		if (sigset_any(self->signals.sigind, SIGNALS_TERMINATING))
		{
			// A CAUGHT signal cuts the nap short instead of ending the
			// program. Returning is what makes delivery possible at all: the
			// handler is armed at the DISPATCHER'S EXIT, so a loop that kept
			// parking would hold the signal forever and never reach it.
			// The nap is not resumed and the remaining time is not slept —
			// SIGNALS.md §8, no restart, no EINTR: an interrupted call says
			// so and the caller decides. os64_sleep answers INTERRUPTED and a
			// program that wanted the whole nap loops.
			// The task comes from the THREAD — cls->task can be a core
			// migration behind (see the dispatcher checkpoint's note), and a
			// sleeper is exactly the thread most likely to wake on a
			// different core than it parked on.
			task_t *owner = (task_t *)self->ownerTask;
			if (signal_has_handler_for_pending(owner, self))
				return (uint64_t)(int64_t)OS64_INTERRUPTED;

			// Nothing will catch it. Ctrl+C (or a ctl write) landed while we
			// napped: die here, in our own context.
			raise_terminating_signal_and_die(owner, self);
			__builtin_unreachable();
		}

		if (kTicksSinceStart >= wakeTick)
			return 0;

		// Park with the wake deadline in sigdata[SIGSLEEP] — sigaction
		// triggers the scheduler itself, so we genuinely leave the CPU here
		// and resume on the next line when woken.
		signal_raise(SIGSLEEP, wakeTick, self);
	}
}

// ticks(out) — hand ring 3 the monotonic clock: tick count since boot plus
// the ACTIVE tick rate, in one call (os64_ticks_t, abi os64/ticks.h — the
// stopwatch/calendar doctrine lives there). One call because the two numbers
// are only useful together: a count without its rate is a number wearing no
// units. First consumers: top's CPU%, sleep's help printing the live
// scheduler interval, uptime for whoever asks.
static uint64_t syscall_ticks(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5;

	os64_ticks_t *user_out = (os64_ticks_t *)arg0;
	if (user_out == NULL)
		return SYSCALL_RESULT_BAD_USER_DATA;   // the whole point is the struct

	os64_ticks_t t;
	t.ticks = kTicksSinceStart;
	t.per_second = TICKS_PER_SECOND;

	if (!copy_to_user_buffer(user_out, &t, sizeof(t)))
		return SYSCALL_RESULT_BAD_USER_DATA;
	return 0;
}

// memory(out) — the physical memory picture, one atomic snapshot. free/used/
// largest come from a single walk of the allocator ledger under its lock
// (allocator_memory_snapshot), so they agree with each other; total/usable
// are boot-time memmap constants. `available` is summed HERE, in the kernel,
// so userland never reinvents Linux's wrong-column arithmetic — and
// `reclaimable` is the future page cache's seat at the table, honestly zero
// until one exists. free + used == usable is a LIVE INVARIANT (see
// os64/memory.h for the audit doctrine); the ring-3 fixture asserts it.
static uint64_t syscall_memory(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5;

	os64_memory_t *user_out = (os64_memory_t *)arg0;
	if (user_out == NULL)
		return SYSCALL_RESULT_BAD_USER_DATA;   // the whole point is the struct

	os64_memory_t m;
	m.total  = kTotalMemory;
	m.usable = kAvailableMemory;
	allocator_memory_snapshot(&m.free, &m.used, &m.largest_free_extent);
	m.reclaimable = 0;                    // no page cache yet — the truth
	m.available   = m.free + m.reclaimable;
	m.page_size   = PAGE_SIZE;

	if (!copy_to_user_buffer(user_out, &m, sizeof(m)))
		return SYSCALL_RESULT_BAD_USER_DATA;
	return 0;
}

// printat(x, y, str) — the widget-plane syscall: park a string at an absolute
// character cell on the physical console. See the abi header for the doctrine
// (widget != console write; lives outside the future VT stack). The handler
// is a bounds check, a string copy, and a handoff to print_at(), which brings
// its own guarantees: no cursor motion, no wrap/scroll, clips at the screen
// edge under the renderer lock, and politely declines while the GUI owns the
// framebuffer.
static uint64_t syscall_printat(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg3; (void)arg4; (void)arg5;

	// One row of a widget, tops. print_at clips at the screen edge anyway, so
	// a bigger buffer would buy nothing but copy time; a "widget" longer than
	// this is console content that took a wrong turn at the API.
	char kernel_buffer[256];

	// Cap the CELL coordinates before print_at multiplies them into pixels:
	// its edge-clip compares px against the framebuffer width AFTER the
	// multiply, so an absurd x could wrap the 32-bit pixel math back onto the
	// screen. No display has 4096 columns; nothing legitimate is lost.
	if (arg0 > 4095 || arg1 > 4095)
		return SYSCALL_RESULT_INVALID;

	if (!copy_user_string((const char *)arg2, kernel_buffer, sizeof(kernel_buffer)))
		return SYSCALL_RESULT_BAD_USER_DATA;

	print_at(&kRenderer, (unsigned int)arg0, (unsigned int)arg1, kernel_buffer);
	return 0;
}

// time(out) — the wall clock's raw truth (see os64/time.h for the doctrine:
// the kernel keeps a counter, the library keeps the calendar). One consistent
// snapshot: epoch and the sub-second phase are advanced by the same timer IRQ,
// so we read epoch / phase / epoch-again and retry if the second rolled in
// between — otherwise a caller could see 12:00:00 paired with the last tick
// of 11:59:59. The loop runs at most twice a second per caller in practice;
// it exists for the once-a-day time a syscall straddles the boundary.
static uint64_t syscall_time(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5;

	os64_time_t *user_out = (os64_time_t *)arg0;
	if (user_out == NULL)
		return SYSCALL_RESULT_BAD_USER_DATA;   // the whole point is the struct

	uint64_t epoch, phase;
	do {
		epoch = kSystemCurrentTime;
		phase = irq0_current_count;
	} while (epoch != kSystemCurrentTime);

	os64_time_t t;
	t.epoch             = (int64_t)epoch;
	t.tz_offset_minutes = kTimeZone * 60;     // kernel config is whole hours
	t.ticks_into_second = (uint32_t)phase;
	t.ticks_per_second  = (uint32_t)kTicksPerSecond;
	t.reserved          = 0;

	if (!copy_to_user_buffer(user_out, &t, sizeof(t)))
		return SYSCALL_RESULT_BAD_USER_DATA;
	return 0;
}

// set_time(epoch) — adjust only the wall clock. An aligned 64-bit store is
// atomic on x86-64 and serializes cleanly with IRQ0's locked increment. We
// preserve the current sub-second phase: the date utility accepts whole
// seconds, and retaining phase avoids racing IRQ0 over a second shared word.
// Uptime and every duration remain on kTicksSinceStart and cannot jump.
static uint64_t syscall_set_time(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5;

	__atomic_store_n(&kSystemCurrentTime, arg0, __ATOMIC_SEQ_CST);
	return 0;
}

// setenv(key, value|NULL) — mutate the calling task's environment (the abi
// header has the doctrine: same physical page the task reads, children
// snapshot at spawn). arg1 deliberately isn't in the dispatcher's pointer
// mask — NULL is a legal value meaning "unset", so the range check happens
// here, inside copy_user_string, only when a value is actually present.
static uint64_t syscall_setenv(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg2; (void)arg3; (void)arg4; (void)arg5;

	core_local_storage_t *cls = get_core_local_storage();
	task_t *task = cls ? cls->task : NULL;
	if (task == NULL || task->env == NULL)
		return SYSCALL_RESULT_INVALID;

	// A key is a NAME, not a novel; a value has to fit the block anyway
	// (one page minus header), so these bounds reject only the absurd.
	char key[128];
	char val[2048];

	if (!copy_user_string((const char *)arg0, key, sizeof(key)))
		return SYSCALL_RESULT_BAD_USER_DATA;
	if (key[0] == '\0')
		return SYSCALL_RESULT_INVALID;      // the empty key names nothing

	if (arg1 == 0)
	{
		uint64_t irq = spinlock_acquire_irqsave(&kTaskEnvLock);
		bool ok = env_unset(task->env, key);
		spinlock_release_irqrestore(&kTaskEnvLock, irq);
		return ok ? 0 : SYSCALL_RESULT_INVALID;
	}

	if (!copy_user_string((const char *)arg1, val, sizeof(val)))
		return SYSCALL_RESULT_BAD_USER_DATA;

	// The whole mutate path holds kTaskEnvLock: growth SWAPS task->env for a
	// bigger block, and env_inherit (a sibling thread spawning) must never
	// memcpy a block that's being freed under it.
	uint64_t irq = spinlock_acquire_irqsave(&kTaskEnvLock);
	bool ok = env_set(task->env, key, val);
	if (!ok)
	{
		// Full block. GROW AND RETRY (2026-08-14): double the block, capped at
		// the fixed-VA window task.h reserves for it — the same window
		// task_setup_entry maps below TASK_EXIT_TRAMPOLINE_VIRT. At the 64KB
		// ceiling env_grow returns NULL and the refusal below is honest AND
		// final, which is what SYSCALL_RESULT_INVALID meant here all along.
		envpage_t *bigger = env_grow(task->env, TASK_ENV_MAX_BYTES / PAGE_SIZE);
		if (bigger != NULL)
		{
			// Swap the task's read-only window over the new block. Only tasks
			// that run an ELF image have the window mapped (task_setup_entry);
			// probe rather than guess. The window's VA never changes, so
			// userland pointers into the environment stay valid as ADDRESSES —
			// content shifting under them is the documented setenv semantic
			// (proc.h: a set invalidates prior getenv/env_next results).
			envpage_t *old = task->env;
			if (task->pml4v != NULL)
			{
				uintptr_t mapped = paging_walk_paging_table((pt_entry_t *)task->pml4v, TASK_ENV_VIRT);
				if (mapped != 0 && mapped != 0xbadbadba)
				{
					// Unmap the OLD page count, map the NEW one — both fit the
					// 2MB page table the argv blob already forced into being,
					// so no table allocation happens under this lock.
					paging_unmap_pages((pt_entry_t *)task->pml4v, TASK_ENV_VIRT,
					                   (size_t)old->page_count * PAGE_SIZE);
					paging_map_pages((pt_entry_t *)task->pml4v, TASK_ENV_VIRT,
					                 (uintptr_t)bigger - kHHDMOffset,
					                 bigger->page_count, PAGE_PRESENT | PAGE_USER);
				}
			}
			task->env = bigger;
			// Freeing the old block rides the HHDM-unmap TLB shootdown, which
			// also flushes any sibling core's stale window translation. The
			// beat between remap and shootdown is benign: a stale entry still
			// points at the old block, whose bytes are an intact snapshot
			// until this kfree.
			kfree(old);
			ok = env_set(task->env, key, val);
		}
	}
	spinlock_release_irqrestore(&kTaskEnvLock, irq);

	// A false here now means the 64KB ceiling itself (or OOM) — surface it.
	return ok ? 0 : SYSCALL_RESULT_INVALID;
}

// klog_read(entries, max) — hand kernel log entries to a userland log
// daemon, oldest-first across every core. Returns the count taken; 0 means
// "nothing right now", which is an ordinary answer, not an error.
//
// This is the mechanism half of the logging split: the kernel keeps the
// rings and the merge order (both need locks and cross-core knowledge no
// program should have), and userland decides where the bytes live. The
// call also CLAIMS the log — see klog_dequeue and os64/klog.h for the
// heartbeat that makes the claim safe to lose.
#define KLOG_READ_MAX_BATCH 64
static uint64_t syscall_klog_read(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg2; (void)arg3; (void)arg4; (void)arg5;

	uint32_t want = (uint32_t)arg1;
	if (want == 0)
		return 0;
	// Bound the batch: the singleton staging buffers below are sized for
	// KLOG_READ_MAX_BATCH, and an unbounded `max` from ring 3 would run
	// straight off their end.
	if (want > KLOG_READ_MAX_BATCH)
		want = KLOG_READ_MAX_BATCH;

	// ONE reader at a time. Reading consumes entries, so a second daemon
	// wouldn't duplicate the log, it would deal it out — two files, each a
	// random half. Refuse the latecomer loudly instead (it should print and
	// exit); the claim lapses by heartbeat if the holder dies, so this can
	// never wedge logging behind a corpse.
	core_local_storage_t *ccls = get_core_local_storage();
	task_t *ctask = ccls ? ccls->task : NULL;
	if (ctask == NULL)
		return SYSCALL_RESULT_INVALID;
	if (!klog_sink_try_claim(ctask->taskID))
	{
		printd(DEBUG_SYSCALL, "klog_read: task %s (%lu) refused — the log sink is claimed by a live reader\n",
		       ctask->exename, ctask->taskID);
		return SYSCALL_RESULT_INVALID;
	}

	// Dequeue into KERNEL memory first, then copy out in one hop. Same
	// discipline as every other read: klog_dequeue runs under the log
	// work-lock, and touching user pages there could demand-page while
	// holding it.
	//
	// SINGLETON staging buffers (2026-08-04, the paging-pool exhaustion
	// hunt): this used to kmalloc/kfree BOTH arrays on EVERY call — and the
	// draining daemon calls this in a tight loop, so under a DEBUG_SCHEDULER
	// soak that was ~100 multi-page allocations a second feeding the
	// allocator's address march (see the pool-sizing comment in paging.c).
	// The exclusivity claim above is what makes a bare static safe: the CAS
	// guarantees ONE reader at a time, ever, so these buffers have exactly
	// one user by construction. Allocated once at max-batch size, kept for
	// the life of the system — a daemon's working set, not a leak.
	static log_entry_t *klogStaged = NULL;
	static os64_logent_t *klogOut = NULL;
	// Each buffer's init stands alone, and the refusal re-checks BOTH: a
	// first call that wins one kmalloc and loses the other to OOM must not
	// strand the loser NULL forever behind its partner's success (the retry
	// path below would otherwise skip init and write through NULL).
	if (klogStaged == NULL)
		klogStaged = kmalloc(KLOG_READ_MAX_BATCH * sizeof(log_entry_t));
	if (klogOut == NULL)
		klogOut = kmalloc(KLOG_READ_MAX_BATCH * sizeof(os64_logent_t));
	if (klogStaged == NULL || klogOut == NULL)
		return SYSCALL_RESULT_INVALID;
	log_entry_t *staged = klogStaged;

	uint32_t got = klog_dequeue(staged, want);
	if (got == 0)
	{
		// Shutdown's retire handshake (log.h): only an EMPTY poll may carry
		// the farewell — every logged byte has already been handed over, so
		// never-drop-a-byte holds to the end. Release the claim HERE, not in
		// some later cleanup: shutdown_system is watching it to know the
		// daemon has been told.
		if (kKlogRetireRequested)
		{
			klog_sink_release();
			return (uint64_t)OS64_KLOG_RETIRED;
		}
		return 0;   // nothing waiting — the daemon sleeps and asks again
	}

	// Translate to the ABI struct. The kernel's log_entry_t carries a TSC
	// and internal padding that userland has no business depending on;
	// this loop is the boundary where the internal shape stops mattering.
	os64_logent_t *out = klogOut;
	for (uint32_t i = 0; i < got; i++)
	{
		out[i].ticks     = staged[i].ticks;
		out[i].threadID  = staged[i].threadID;
		out[i].core      = staged[i].core_id;
		out[i].level     = staged[i].log_level;
		out[i].category  = staged[i].category;
		out[i].continued = staged[i].continued ? 1 : 0;
		out[i].reserved[0] = out[i].reserved[1] = out[i].reserved[2] = 0;
		memcpy(out[i].message, staged[i].message, OS64_LOG_MESSAGE_MAX);
		out[i].message[OS64_LOG_MESSAGE_MAX - 1] = '\0';
	}

	bool ok = copy_to_user_buffer((void *)arg0, out, got * sizeof(os64_logent_t));
	return ok ? (uint64_t)got : SYSCALL_RESULT_BAD_USER_DATA;
}

// thread(entry, arg, exit_stub) — os64's first ring-3 threads.
//
// Everything the new thread needs already existed: createThread gives it
// its own guarded user and kernel stacks at unique task-local addresses,
// and the scheduler has always scheduled THREADS. All that was missing
// was a way to ask. See os64/syscall_numbers.h for the API argument and
// thread_join.h for why the handle points at a join object.
static uint64_t syscall_thread(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg3; (void)arg4; (void)arg5;

	core_local_storage_t *cls = get_core_local_storage();
	task_t *task = cls ? cls->task : NULL;
	if (task == NULL)
		return SYSCALL_RESULT_INVALID;

	// A null entry or exit stub is a caller bug, not a thread.
	if (arg0 == 0 || arg2 == 0)
	{
		printd(DEBUG_THREAD, "syscall_thread: %s passed entry=0x%lx exit_stub=0x%lx — refused\n",
		       task->exename, arg0, arg2);
		return SYSCALL_RESULT_INVALID;
	}
	// Both must live in the caller's own (lower-half) address space. A
	// ring-3 thread that starts executing in the kernel's half is the
	// whole reason ring 3 exists.
	if (arg0 >= kHHDMOffset || arg2 >= kHHDMOffset)
	{
		printd(DEBUG_THREAD, "syscall_thread: %s passed a non-user address (entry=0x%lx stub=0x%lx) — refused\n",
		       task->exename, arg0, arg2);
		return SYSCALL_RESULT_BAD_USER_DATA;
	}

	thread_join_t *j = thread_join_create(task, arg0, arg1, arg2);
	if (j == NULL)
		return SYSCALL_RESULT_INVALID;

	int h = handle_alloc(task, HANDLE_THREAD, j);
	if (h < 0)
	{
		// Out of handle slots. The thread is ALREADY RUNNING — it was
		// submitted to the scheduler inside thread_join_create — so this
		// drops the handle's reference and lets it run detached rather
		// than pretending it never started.
		printd(DEBUG_THREAD, "syscall_thread: %s out of handles; thread 0x%08lx runs detached\n",
		       task->exename, j->threadID);
		thread_join_close(j);
		return SYSCALL_RESULT_INVALID;
	}

	printd(DEBUG_THREAD, "syscall_thread: %s got handle %d for thread 0x%08lx\n",
	       task->exename, h, j->threadID);
	return (uint64_t)h;
}

// thread_exit(retval) — end THIS thread, leaving the task alive.
static uint64_t syscall_thread_exit(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5;

	core_local_storage_t *cls = get_core_local_storage();
	thread_t *self = cls ? cls->currentThread : NULL;
	task_t *task = cls ? cls->task : NULL;
	if (self == NULL || task == NULL)
		return SYSCALL_RESULT_INVALID;

	printd(DEBUG_THREAD, "syscall_thread_exit: thread 0x%08lx of %s exiting with %ld\n",
	       self->threadID, task->exename, (int64_t)arg0);

	// Publish the answer and drop the thread's reference BEFORE leaving the
	// run queue: after the state change below this thread never executes
	// another instruction, so anything left undone stays undone forever.
	thread_join_finish(self->threadID, (int64_t)arg0);

	// MARK, don't move. The scheduler takes the current thread off the CPU
	// itself and, seeing `exited`, files it under ZOMBIE (scheduler.c's
	// take-off-CPU branch). Doing the queue surgery here instead removed
	// this thread from qRunning BEFORE the scheduler went looking for it,
	// and it panicked: "Can't find thread with id 63 in running queue".
	// The thread that is currently executing must still BE in the running
	// queue when the scheduler takes over — that is how it finds itself.
	self->retVal = (uint64_t)arg0;
	self->exited = true;

	// Off the run queue and never coming back: hand the core to someone
	// else. scheduler_trigger does not return for a thread in this state.
	scheduler_trigger(NULL);
	while (1)
		__asm__ volatile("hlt");   // unreachable; the scheduler owns us now
}

// sync(handle) — make what a program has written VISIBLE and durable.
// See the ABI header for why this is its own verb rather than something
// write() does implicitly: syncing on every write would tax every file
// writer in the system to serve the one program that actually needs its
// bytes readable by others while it still holds the file open.
static uint64_t syscall_sync(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5;

	core_local_storage_t *cls = get_core_local_storage();
	task_t *task = cls ? cls->task : NULL;
	if (task == NULL)
		return SYSCALL_RESULT_INVALID;

	handle_t *h = handle_get(task, (int)(int64_t)arg0);
	if (h == NULL || h->type != HANDLE_FILE)
		return SYSCALL_RESULT_INVALID;   // pipes and consoles have nothing to commit

	// Runs under kKernelPML4 like every other filesystem operation — the
	// FAT layer's DMA mappings live there.
	file_io_params_t *fp = kmalloc(sizeof(*fp));
	if (fp == NULL)
		return SYSCALL_RESULT_INVALID;
	fp->file = (vfs_file_t *)h->object;
	fp->buf = NULL;
	fp->len = 0;
	fp->result = -1;
	call_in_kernel_context(file_do_sync, fp);

	long rc = fp->result;
	kfree(fp);
	return (rc < 0) ? SYSCALL_RESULT_INVALID : 0;
}

// SYSCALL_SYNC_ALL — sync(1)'s engine. Takes nothing, syncs every open file
// on every mount via the VFS registry, returns the count synced (0 is an
// honest answer on an idle system). Same kernel-context recipe as the
// per-handle sync above, for the same reason: the FAT layer's DMA mappings
// live under kKernelPML4.
static uint64_t syscall_sync_all(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg0; (void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5;

	core_local_storage_t *cls = get_core_local_storage();
	task_t *task = cls ? cls->task : NULL;
	if (task == NULL)
		return SYSCALL_RESULT_INVALID;

	file_io_params_t *fp = kmalloc(sizeof(*fp));
	if (fp == NULL)
		return SYSCALL_RESULT_INVALID;
	fp->file = NULL;
	fp->buf = NULL;
	fp->len = 0;
	fp->result = -1;
	call_in_kernel_context(file_do_sync_all, fp);

	long rc = fp->result;
	kfree(fp);
	return (rc < 0) ? SYSCALL_RESULT_INVALID : (uint64_t)rc;
}

// shutdown(8)'s engine — the contract and lineage live in syscall_numbers.h,
// the ordered descent lives in shutdown.c. No arguments to validate, no
// return to marshal: this call ends with the power, not with sysret.
static uint64_t syscall_shutdown(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5;

	// arg0 picks the door: 0 = power off, 1 = reboot. Anything else is
	// REFUSED rather than guessed — an unknown mode on a call that never
	// returns is the worst possible moment to be charitable about input.
	if (arg0 != OS64_SHUTDOWN_POWEROFF && arg0 != OS64_SHUTDOWN_REBOOT)
		return SYSCALL_RESULT_INVALID;

	shutdown_system((os64_shutdown_mode_t)arg0);
}

// getpid() — contract and lineage in syscall_numbers.h. The caller IS the
// current task, so the answer is sitting in CLS; the only care taken is the
// same no-task guard every introspective path carries (a ring-3 caller
// always has a task, but this handler must not be the one place that
// assumes it). Cannot fail: an identity crisis is not an errno.
static uint64_t syscall_getpid(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg0; (void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5;
	core_local_storage_t *cls = get_core_local_storage();
	return (cls != NULL && cls->task != NULL) ? cls->task->taskID : 0;
}

// tty_handle() — "a handle on MY terminal, whatever handle 0 became."
//
// The pagers' door (full contract at SYSCALL_TTY_HANDLE in the ABI header).
// `ps | less` needs its DOCUMENT from the pipe on handle 0 and its KEYS from
// the terminal; redirection can only point one slot at one thing, so the
// terminal needs a second name. Unix spells that name /dev/tty; os64 spells
// it as a verb until a devfs exists to make the name honest.
//
// Sibling of getpid above, and the resemblance is the point: both answer a
// question about the CALLER, take no arguments, and cannot fail for any
// reason except the handle table being full. There is no tty to look up here
// — a HANDLE_CONSOLE_IN carries no object, and the read path resolves
// task_tty(caller) at every read (console.c). That late binding is what makes
// one tag serve a VT and a pty slave alike: the handle means "my terminal",
// so a pager inside a terminal WINDOW reads the slave without one line of
// special case. Minting a second reference is therefore just handle_alloc();
// the hard part was true before this call existed.
//
// Not exclusive, and deliberately so: the caller now shares one input ring
// with whoever else holds a console handle on the same terminal. That is the
// same ring handle 0 has always read, and the same competition `cat | cat`
// could already arrange — the doorway is new, the room is not.
static uint64_t syscall_tty_handle(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg0; (void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5;

	core_local_storage_t *cls = get_core_local_storage();
	task_t *task = cls ? cls->task : NULL;
	if (task == NULL)
		return SYSCALL_RESULT_INVALID;

	// handle_alloc starts its search at slot 3, so this can never land on a
	// standard stream and silently redirect the caller's own stdin.
	int h = handle_alloc(task, HANDLE_CONSOLE_IN, NULL);
	if (h < 0)
		return SYSCALL_RESULT_INVALID;   // table full — the only failure there is

	printd(DEBUG_SYSCALL, "tty_handle: task %s got handle %d on tty %u\n",
	       task->exename, h, task_tty(task) ? task_tty(task)->index + 1 : 1);
	return (uint64_t)h;
}

// conf_resolve — where is the config file called <name>? Contract in
// abi/os64/syscall_numbers.h; the walk itself is conf.c's.
//
// The kernel does the walking because a ladder every reader obeys must be
// parsed by exactly one thing, and because the walker being the resolver is
// what lets /sys/conf report which file each reader actually took without a
// second channel for saying so.
//
// conf_find takes the kernel-context trampoline for its probe, so this
// handler needs no CR3 arrangement of its own — the SYSCALL_DEFINE row asks
// for none.
static uint64_t syscall_conf_resolve(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg5;

	const char *user_name = (const char *)arg0;
	char       *user_out  = (char *)arg1;
	size_t      cap       = (size_t)arg2;
	size_t      from      = (size_t)arg3;
	bool        any       = (arg4 != 0);   // don't probe — just build the path

	core_local_storage_t *cls = get_core_local_storage();
	task_t *task = cls ? cls->task : NULL;
	if (task == NULL)
		return SYSCALL_RESULT_INVALID;
	if (user_out == NULL || cap == 0)
		return SYSCALL_RESULT_BAD_USER_DATA;

	char name[CONF_NAME_MAX];
	if (!copy_user_string(user_name, name, sizeof(name)))
		return SYSCALL_RESULT_BAD_USER_DATA;

	char found[CONF_PATH_MAX];
	int matched;
	if (any) {
		// "Where WOULD this go?" — the writer's question. No probe, because
		// the file it is about to create does not exist yet by definition.
		if (!conf_path_at(from, name, found, sizeof(found)))
			return SYSCALL_RESULT_INVALID;
		matched = (int)from;
	} else {
		matched = conf_find_from(name, from, found, sizeof(found));
		if (matched < 0)
			return SYSCALL_RESULT_INVALID;   // nowhere left — caller uses its defaults
	}

	// Refuse rather than truncate. A HALF path is worse than no path: it
	// opens nothing, or — far worse on a system with a curated tree — opens
	// something else. The caller sized the buffer; tell it the size was wrong.
	size_t len = 0;
	while (found[len] != '\0')
		len++;
	if (len + 1 > cap)
		return SYSCALL_RESULT_BAD_USER_DATA;

	if (!copy_to_user_buffer(user_out, found, len + 1))
		return SYSCALL_RESULT_BAD_USER_DATA;

	printd(DEBUG_SYSCALL, "conf_resolve: task %s: '%s' -> '%s' (ladder %d)\n",
	       task->exename, name, found, matched);
	// The MATCHED INDEX PLUS ONE, so success is always >= 1 and can never be
	// confused with SYSCALL_RESULT_INVALID (all ones). Hand it straight back
	// as the next call's `from` to walk to the following copy — which is how
	// the resolver reads every hosts file instead of only the first.
	return (uint64_t)(matched + 1);
}

// heap_report(ptr) — "my heap's report card lives here."
//
// The kernel does nothing with the address but REMEMBER it. Nobody reads it
// until somebody opens /proc/<id>/heap, and that reader (procfs.c) walks this
// task's own page tables to get at it — never a bare dereference, because a
// user VA means nothing under any other CR3 and means something WRONG under
// the vestigial low identity window (the bug that once made husk's cmdline
// report "/idle7"; see proc_copy_task_string's comment for the whole story).
//
// The dispatcher has already range-checked arg0 as a user pointer (mask 0x01),
// so all that remains is the alignment the struct's uint64 fields require.
static uint64_t syscall_heap_report(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5;

	core_local_storage_t *cls = get_core_local_storage();
	task_t *task = cls ? cls->task : NULL;

	if (task == NULL)
		return SYSCALL_RESULT_INVALID;

	if ((arg0 & 0x7) != 0)
		return SYSCALL_RESULT_INVALID;   // an 8-aligned struct, or nothing

	task->heapReportVirt = (uintptr_t)arg0;

	printd(DEBUG_SYSCALL, "heap_report: task %s publishes its heap report at 0x%016lx\n",
	       task->exename, (uint64_t)arg0);
	return 0;
}

// ── The GUI rows (16-21) — GRAPHICS.md's userland boundary, step 2 ──────────
// Thin translation shims, deliberately: copy what crosses the ring boundary,
// then call the SAME gui_client.h functions the kernel's own clients
// (guicomp, the console, the demos) call directly — ownership checks,
// locking, and every piece of real logic live there, written once. Handlers
// run under the caller's CR3 like every row in this table
// (needs_cr3_switch=false): GUI state is upper-half and visible from any
// address space, and the user pointers being copied are lower-half and
// visible only from THIS one.

static uint64_t syscall_gui_window_create(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	// arg0 = title (mask 0x01), arg1/arg2 = x/y (signed), arg3/arg4 = w/h,
	// arg5 = flags. A title longer than the window's own field is REFUSED,
	// not truncated — the same convention every string crossing this
	// boundary follows (a silently shortened name "succeeded" and didn't).
	char title[GUI_WINDOW_TITLE_MAX];
	if (!copy_user_string((const char *)arg0, title, sizeof(title)))
		return (uint64_t)GUI_ERR_BAD_ARGS;

	return (uint64_t)gui_window_create(title, (int32_t)arg1, (int32_t)arg2,
	                                   (uint32_t)arg3, (uint32_t)arg4, arg5);
}

static uint64_t syscall_gui_window_destroy(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5;
	return (uint64_t)gui_window_destroy((int64_t)arg0);
}

static uint64_t syscall_gui_window_get_surface(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg2; (void)arg3; (void)arg4; (void)arg5;
	if (arg1 == 0)
		return (uint64_t)GUI_ERR_BAD_ARGS;

	surface_t s;
	int64_t rc = gui_window_get_surface((int64_t)arg0, &s);
	if (rc != 0)
		return (uint64_t)rc;

	// The surface pivot delivered: gui_window_get_surface answers per
	// window flavor, and for a ring-3 caller's (task-backed) window that
	// is the TASK's own VA for the canvas — a pointer it can finally draw
	// through. (This is where a NULL stood between steps 2 and 3.)
	if (!copy_to_user_buffer((void *)arg1, &s, sizeof(s)))
		return (uint64_t)GUI_ERR_BAD_ARGS;
	return 0;
}

// signal_handler — install a handler for a signal, answer with the previous.
// Contract in abi/os64/syscall_numbers.h; the argument for putting the table
// on the TASK is SIGNALS.md §2.
//
// REGISTRATION ONLY. Nothing is delivered to ring 3 until the frame-and-
// trampoline half lands (step 3), so today an installed handler means exactly
// "do not apply the default action" — which the forced-syscall push in
// scheduler.c has honoured for SIGINT since before there was a way to install
// one. That is deliberate: a program can be written against this now, and it
// will start actually running its handler when delivery arrives, without the
// interface changing under it.
static uint64_t syscall_signal_handler(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg2; (void)arg3; (void)arg4; (void)arg5;

	// The task comes from the THREAD (the arc's rule, and this is a DECISION
	// site — it writes a handler table; installing into cls->task during the
	// old staleness window would have armed the wrong program).
	core_local_storage_t *cls = get_core_local_storage();
	thread_t *thread = cls ? cls->currentThread : NULL;
	task_t *task = thread ? (task_t *)thread->ownerTask : NULL;
	if (task == NULL)
		return (uint64_t)(int64_t)OS64_SIG_ERR_BAD_SIGNAL;

	int signo = (int)(int64_t)arg0;
	void *handler = (void *)arg1;

	if ((int)signo <= 0 || signo >= SIGNAL_COUNT)
		return (uint64_t)(int64_t)OS64_SIG_ERR_BAD_SIGNAL;
	if (!signal_is_catchable((signals)signo))
		return (uint64_t)(int64_t)OS64_SIG_ERR_UNCATCHABLE;

	// A handler must be an address ring 3 could actually execute. The kernel
	// is about to point a thread's RIP at this, so a higher-half value would
	// have the CPU attempt kernel text at CPL 3 — it faults harmlessly, but
	// refusing it here names the mistake instead of turning it into a
	// segfault three steps later. NULL is the exception: it means "default".
	if (handler != NULL && (uint64_t)handler >= TASK_HEAP_END)
		return (uint64_t)(int64_t)OS64_SIG_ERR_BAD_HANDLER;

	void *previous = signal_set_handler(task, (signals)signo, handler);
	return (uint64_t)previous;
}

// sigreturn — resume what a handler interrupted. A program never calls this
// deliberately; the stub at TASK_SIGRETURN_VIRT does, when the handler `ret`s.
//
// THIS IS THE MOST ATTACKABLE CALL IN THE SIGNAL PATH, because it is a
// "restore register state" primitive reached from ring 3. So the frame is
// VALIDATED, never trusted:
//
//   - it must carry the kernel's magic (the cheap catch for an honest bug);
//   - a handler for that signal must actually be running (sigmask), which is
//     what stops a program calling sigreturn out of nowhere to install a
//     register state of its choosing;
//   - and RFLAGS is SANITIZED, never trusted. "The frame was written by us"
//     is not a defensible claim for any of its contents: the frame sits on
//     the user's own writable stack, so every word in it is ring 3's to
//     forge — and sysretq loads RFLAGS from R11 nearly verbatim, IF and
//     IOPL included. A forged IF=0 would park a core beyond the timer's
//     reach forever; IOPL=3 would hand ring 3 the I/O ports. So the frame's
//     rflags keeps only the bits a user program owns and the rest are
//     forced (SIGNAL_RFLAGS_* in signals.h, where §10's iretq-shaped
//     sigreturn is told to inherit the same mask).
//
// A stack-range check on the frame POINTER is deliberately absent: it would
// prove nothing, because the frame's CONTENTS are user-writable wherever it
// sits. The sanitization is the defence; the pointer's location is not.
//
//   - and the saved RIP/RSP must be CANONICAL LOWER-HALF addresses. This
//     corrects a claim an earlier version of this comment made — that RIP
//     and RSP "are not range-checked on purpose" because a bad one "faults
//     in ring 3 as its own segfault." That is FALSE for a NONCANONICAL
//     address: SYSRETQ (the short path) loads RIP from RCX and IRETQ (the
//     full path) pops it from the frame, and BOTH raise #GP in RING 0 when
//     handed a noncanonical value — before the privilege drop completes. So
//     a handler that forges saved.rip could crash the KERNEL on the next
//     signal it receives (the CVE-2012-0217 family). Requiring both below
//     the canonical boundary rejects the noncanonical range AND forces the
//     addresses into user space, where a genuinely bad-but-canonical one is
//     back to being the program's own ring-3 fault. (USER_CANONICAL_MAX,
//     the boundary constant, lives in paging.h — shared with the delivery
//     paths and paging_resolve_user_writable.)
static uint64_t syscall_sigreturn(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5;

	core_local_storage_t *cls = get_core_local_storage();
	thread_t *thread = cls ? cls->currentThread : NULL;
	// The task comes from the THREAD — the arc's own rule, applied to its
	// own syscalls too. cls->task is coherent again since scheduler_load_thread
	// pairs the stores, but a decision site should not have to know that.
	task_t   *task   = thread ? (task_t *)thread->ownerTask : NULL;
	if (task == NULL || thread == NULL)
		return SYSCALL_RESULT_INVALID;

	// The THREAD's own frame pointer (see thread.h): we are inside the
	// sigreturn syscall right now, so this names sigreturn's own 40-byte
	// return frame — the one sysretq will rebuild this thread from.
	uint64_t *frame = (uint64_t *)thread->syscall_return_frame;
	if (frame == NULL)
		return SYSCALL_RESULT_INVALID;   // not on a syscall return path

	signal_frame_t saved;
	if (!copy_user_buffer((const void *)arg0, &saved, sizeof(saved)))
		return SYSCALL_RESULT_BAD_USER_DATA;

	if (saved.magic != SIGNAL_FRAME_MAGIC && saved.magic != SIGNAL_FRAME_MAGIC_FULL)
	{
		printd(DEBUG_SIGNALS, "sigreturn: %s handed a frame with no magic — refused\n",
		       task->exename);
		return SYSCALL_RESULT_BAD_USER_DATA;
	}
	if (saved.signo == 0 || saved.signo >= SIGNAL_COUNT ||
	    !sigset_has(thread->signals.sigmask, (signals)saved.signo))
	{
		// No handler for that signal is running on this thread, so there is
		// nothing to return FROM. This is the check that makes the call
		// useless to anyone who did not get here the intended way.
		printd(DEBUG_SIGNALS, "sigreturn: %s is not inside a handler for signal %lu — refused\n",
		       task->exename, saved.signo);
		return SYSCALL_RESULT_BAD_USER_DATA;
	}

	// CANONICAL LOWER-HALF or nothing (see the block comment). rip/rsp are the
	// §5 frame's fields and the full frame shares them by prefix, so this one
	// check guards both the sysretq and the iretq return. A noncanonical value
	// here is a KERNEL #GP waiting to happen, not a ring-3 segfault.
	if (saved.rip >= USER_CANONICAL_MAX ||
	    saved.rsp >= USER_CANONICAL_MAX)
	{
		printd(DEBUG_SIGNALS,
		       "sigreturn: %s handed a noncanonical rip/rsp (%p/%p) — refused\n",
		       task->exename, (void *)saved.rip, (void *)saved.rsp);
		return SYSCALL_RESULT_BAD_USER_DATA;
	}

	// ── The FULL frame (§10): the handler interrupted a SPIN, not a syscall ─
	//
	// The scheduler delivered this one (signal_deliver_to_regs), so the
	// interrupted context is an arbitrary instruction with every register
	// live, and the road home is the one a preempted thread always takes:
	// thread->regs, loaded by scheduler_load_thread, resumed by iretq. A
	// sysretq return is structurally impossible here — it cannot restore
	// fifteen registers — so THIS CALL NEVER RETURNS: it writes the file
	// into regs, marks them crafted (exec's own seam), and parks. The
	// kernel continuation below this frame is abandoned exactly as exec
	// abandons one.
	if (saved.magic == SIGNAL_FRAME_MAGIC_FULL)
	{
		signal_frame_full_t full;
		if (!copy_user_buffer((const void *)arg0, &full, sizeof(full)))
			return SYSCALL_RESULT_BAD_USER_DATA;

		// TOCTOU: the checks above ran on the FIRST copy (`saved`), but this is
		// a SECOND read of the same user-writable memory — and in a
		// multithreaded task a sibling can rewrite the frame in between,
		// substituting a noncanonical rip/rsp that the first validation never
		// saw (Codex #29 rd2). So re-validate everything we are about to trust
		// FROM THIS SNAPSHOT, and restore only from it. magic, the running-
		// handler signal, and the canonical target — all read out of `full`.
		if (full.base.magic != SIGNAL_FRAME_MAGIC_FULL ||
		    full.base.signo == 0 || full.base.signo >= SIGNAL_COUNT ||
		    !sigset_has(thread->signals.sigmask, (signals)full.base.signo) ||
		    full.base.rip >= USER_CANONICAL_MAX ||
		    full.base.rsp >= USER_CANONICAL_MAX)
		{
			printd(DEBUG_SIGNALS,
			       "sigreturn(full): %s frame changed under validation — refused\n",
			       task->exename);
			return SYSCALL_RESULT_BAD_USER_DATA;
		}

		// Unblock the signal now that its handler has finished (§7) — the
		// signal named by the VALIDATED snapshot, not the earlier copy.
		sigset_del(&thread->signals.sigmask, (signals)full.base.signo);

		// The register file, wholesale — through the same RFLAGS mask as the
		// short path (the frame is the user's to forge; see the block
		// comment above), and with the selectors from GDT CONSTANTS, never
		// from the frame: this context resumes by iretq, which swallows
		// CS/SS whole, and there is exactly one correct answer for a ring-3
		// thread anyway.
		thread->regs.RAX    = full.base.rax;
		thread->regs.RBX    = full.rbx;
		thread->regs.RCX    = full.rcx;
		thread->regs.RDX    = full.rdx;
		thread->regs.RSI    = full.rsi;
		thread->regs.RDI    = full.rdi;
		thread->regs.RBP    = full.rbp;
		thread->regs.R8     = full.r8;
		thread->regs.R9     = full.r9;
		thread->regs.R10    = full.r10;
		thread->regs.R11    = full.r11;
		thread->regs.R12    = full.r12;
		thread->regs.R13    = full.r13;
		thread->regs.R14    = full.r14;
		thread->regs.R15    = full.r15;
		thread->regs.RIP    = full.base.rip;
		thread->regs.RSP    = full.base.rsp;
		thread->regs.RFLAGS = (full.base.rflags & SIGNAL_RFLAGS_USER_BITS) | SIGNAL_RFLAGS_FORCED;
		thread->regs.CS     = GDT_USER_CODE_ENTRY << 3 | 3;
		thread->regs.SS     = GDT_USER_DATA_ENTRY << 3 | 3;
		thread->regs.DS     = GDT_USER_DATA_ENTRY << 3 | 3;
		thread->regs.ES     = GDT_USER_DATA_ENTRY << 3 | 3;
		thread->regs.FS     = GDT_USER_DATA_ENTRY << 3 | 3;
		thread->regs.GS     = GDT_USER_DATA_ENTRY << 3 | 3;

		// This syscall's return frame dies with the abandoned continuation —
		// clear the pointer so nothing can ever trust it (syscall.S's own
		// exit clear will never run for us).
		thread->syscall_return_frame = 0;

		printd(DEBUG_SIGNALS, "sigreturn: %s resumes its spin at %p after signal %lu\n",
		       task->exename, (void *)full.base.rip, saved.signo);

		// Crafted regs must SURVIVE the next store pass — exec's seam: the
		// scheduler skips one save when this is set (see scheduler.c).
		thread->execDontSaveRegisters = true;

		// Park through the ordinary SIGSLEEP machinery with a wake tick of
		// NOW: the next pass takes us off the CPU (store skipped, regs
		// intact), the sleep sweep wakes us the same pass or the next, and
		// the dispatch after that loads the crafted context and iretqs into
		// the stub's caller — the interrupted spin. The loop is sleep's own
		// shape; it never logically exits, because the continuation standing
		// here is never resumed.
		for (;;)
			signal_raise(SIGSLEEP, kTicksSinceStart, thread);
		__builtin_unreachable();
	}

	// Unblock the signal now that its handler has finished (SIGNALS.md §7).
	sigset_del(&thread->signals.sigmask, (signals)saved.signo);

	// Put the interrupted context back where sysretq will find it, and hand
	// the original syscall's answer back in RAX — which is this syscall's
	// return value, because RAX is exactly what a syscall returns in.
	// RFLAGS goes through the mask (see the block comment above and
	// SIGNAL_RFLAGS_* in signals.h): user bits kept, IF forced on, IOPL
	// forced to 0 — sysretq would otherwise hand ring 3 whatever the frame
	// claims, and the frame is the user's to claim things in.
	frame[0] = (saved.rflags & SIGNAL_RFLAGS_USER_BITS) | SIGNAL_RFLAGS_FORCED;
	frame[1] = saved.rip;
	frame[2] = saved.rsp;

	printd(DEBUG_SIGNALS, "sigreturn: %s resumes %p after signal %lu\n",
	       task->exename, (void *)saved.rip, saved.signo);
	return saved.rax;
}

// Where is my window, and what state is it in? The readback half of create —
// see os64/gui.h for why an app needs it (everything the USER does to a
// window happens in the window system, and until this existed no app could
// learn any of it, so none could save what you had arranged).
static uint64_t syscall_gui_window_get_state(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg2; (void)arg3; (void)arg4; (void)arg5;
	if (arg1 == 0)
		return (uint64_t)GUI_ERR_BAD_ARGS;

	os64_gui_window_state_t st;
	int64_t rc = gui_window_get_state((int64_t)arg0, &st);
	if (rc != 0)
		return (uint64_t)rc;

	if (!copy_to_user_buffer((void *)arg1, &st, sizeof(st)))
		return (uint64_t)GUI_ERR_BAD_ARGS;
	return 0;
}

static uint64_t syscall_gui_window_publish(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg2; (void)arg3; (void)arg4; (void)arg5;

	// The damage rect is NULLABLE (NULL = the whole content), so arg1 stays
	// out of the dispatcher's pointer mask — the SETENV precedent — and the
	// copy below is the validation for the non-NULL case.
	rect_t local;
	const rect_t *damage = NULL;
	if (arg1 != 0) {
		if (!copy_user_buffer((const void *)arg1, &local, sizeof(local)))
			return (uint64_t)GUI_ERR_BAD_ARGS;
		damage = &local;
	}

	return (uint64_t)gui_window_publish((int64_t)arg0, damage);
}

static uint64_t syscall_gui_event_poll(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg2; (void)arg3; (void)arg4; (void)arg5;
	if (arg1 == 0)
		return (uint64_t)GUI_ERR_BAD_ARGS;

	// Popped into a kernel local FIRST, copied out only on success — the
	// queue must never lose an event to a bad destination pointer... which
	// is why the copy failing after a successful pop still returns
	// BAD_ARGS: the event is gone either way, and pretending otherwise
	// would be worse. A caller handing in an unmapped buffer has larger
	// problems than one dropped keystroke.
	input_event_t ev;
	int64_t rc = gui_event_poll((int64_t)arg0, &ev);
	if (rc == 1 && !copy_to_user_buffer((void *)arg1, &ev, sizeof(ev)))
		return (uint64_t)GUI_ERR_BAD_ARGS;
	return (uint64_t)rc;
}

static uint64_t syscall_gui_screen_info(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg2; (void)arg3; (void)arg4; (void)arg5;

	// Both out-pointers are nullable — ask for either dimension or both —
	// so neither sits in the mask; each copy validates its own target.
	uint32_t w = 0, h = 0;
	int64_t rc = gui_screen_info(&w, &h);
	if (rc != 0)
		return (uint64_t)rc;

	if (arg0 != 0 && !copy_to_user_buffer((void *)arg0, &w, sizeof(w)))
		return (uint64_t)GUI_ERR_BAD_ARGS;
	if (arg1 != 0 && !copy_to_user_buffer((void *)arg1, &h, sizeof(h)))
		return (uint64_t)GUI_ERR_BAD_ARGS;
	return 0;
}

static uint64_t syscall_gui_event_wait(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg2; (void)arg3; (void)arg4; (void)arg5;
	if (arg1 == 0)
		return (uint64_t)GUI_ERR_BAD_ARGS;

	// Blocks inside the handler exactly the way read() does — sleeping in a
	// syscall on the caller's CR3 is long-established ground. Same
	// copy-out-after-pop rule as poll: the event left the queue either way,
	// and a caller with an unmapped buffer has larger problems than one
	// dropped keystroke.
	input_event_t ev;
	int64_t rc = gui_event_wait((int64_t)arg0, &ev);
	if (rc == 1 && !copy_to_user_buffer((void *)arg1, &ev, sizeof(ev)))
		return (uint64_t)GUI_ERR_BAD_ARGS;
	return (uint64_t)rc;
}
