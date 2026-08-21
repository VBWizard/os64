#ifndef OS64_PROCFS_H
#define OS64_PROCFS_H

// Typed readers for os64's text /proc reports.  The text files remain the
// ABI; this is merely the shared, ignore-unknown-keys parser that keeps every
// process tool from growing its own slightly different interpretation.

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define OS64_PROC_NAME_MAX 64
#define OS64_PROC_COMMAND_MAX 256

typedef enum {
    OS64_PROC_NONE,
    OS64_PROC_RUNNING,
    OS64_PROC_RUNNABLE,
    OS64_PROC_STOPPED,
    OS64_PROC_USLEEP,
    OS64_PROC_ISLEEP,
    OS64_PROC_ZOMBIE
} os64_proc_state_t;

typedef struct {
    uint64_t pid;
    uint64_t ppid;
    uint64_t runtime_us;
    uint64_t minor_faults;
    uint64_t major_faults;
    uint64_t switches;
    uint32_t tty;
    uint32_t core;
    uint32_t threads;
    os64_proc_state_t state;
    uint64_t heap;
    bool kernel;
    bool foreground;
    bool shell;
    char name[OS64_PROC_NAME_MAX];
    char command[OS64_PROC_COMMAND_MAX];
} os64_proc_info_t;

typedef struct {
    uint64_t tid;
    uint64_t pid;
    uint64_t runtime_us;
    uint32_t core;
    os64_proc_state_t state;
    char affinity[24];
} os64_thread_info_t;

// The controlling terminal, via /proc/self/tty — os64's TIOCGWINSZ, shaped
// as a file. rows/cols are what a full-screen program lays itself out in;
// focused lets it skip redraw work while nobody is looking.
typedef struct {
    uint32_t tty;         // 1-based terminal number (Alt+F1 = tty 1)
    uint32_t rows, cols;  // live-screen geometry, in character cells
    uint32_t scrollback;  // history lines currently reachable via Shift+PgUp
    uint64_t fg_task;     // whom Ctrl+C on this terminal would hit (0 = none)
    bool     focused;     // the glass is showing this terminal right now
    bool     live;        // a shell is seated here (false = dormant)
} os64_tty_info_t;

// Snapshot up to capacity live tasks, sorted by the /proc directory's PID
// order. A task disappearing between readdir and open is an ordinary race and
// is skipped. Returns the count, or -1 if /proc itself cannot be opened.
int32_t os64_proc_snapshot(os64_proc_info_t *out, size_t capacity);

// Read one task or thread report. These are useful to refreshers such as top,
// which already own an identity-keyed cache and should not allocate a second
// whole-system snapshot merely to share the parser.
int32_t os64_proc_read(uint64_t pid, os64_proc_info_t *out);
int32_t os64_proc_read_thread(uint64_t pid, uint64_t tid,
                              os64_thread_info_t *out);

// Read the calling process's controlling terminal (via /proc/self/tty).
// Returns 0 on success, -1 if the file cannot be opened or parsed.
int32_t os64_tty_read(os64_tty_info_t *out);

// Snapshot one task's threads. Returns the count, or -1 if the task/thread
// directory has disappeared.
int32_t os64_proc_threads(uint64_t pid, os64_thread_info_t *out,
                          size_t capacity);

const char *os64_proc_state_name(os64_proc_state_t state);
char os64_proc_state_letter(os64_proc_state_t state);

#endif
