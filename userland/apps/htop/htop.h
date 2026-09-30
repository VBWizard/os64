#ifndef HTOP_H
#define HTOP_H
#include "os64/os64.h"
#include "os64/monitor.h"

#define HT_TASKS 512
#define HT_THREADS 512
#define HT_CORES 128
#define HT_ROWS (HT_TASKS + HT_THREADS)
#define HT_WIDTH 240
#define HT_HEIGHT 100
#define HT_HISTORY 48

// The model owns indices and identities, not pointers into older snapshots.
// A sample is borrowed until the next monitor sample; rebuild before drawing.
typedef enum { HT_CPU, HT_HEAP, HT_TIME, HT_PID, HT_NAME, HT_SORT_COUNT } ht_sort_t;
typedef enum { HT_LIST, HT_FILTER, HT_HELP, HT_DETAILS, HT_CONFIRM } ht_mode_t;
typedef enum { HT_NORMAL, HT_DIM, HT_CYAN, HT_GREEN, HT_AMBER, HT_RED,
               HT_TITLE, HT_SELECTED, HT_COLUMN, HT_STYLE_COUNT } ht_style_t;
typedef struct { unsigned char ch, style; } ht_cell_t;
typedef struct { uint16_t task, thread, depth; bool is_thread; } ht_row_t;
typedef struct { uint64_t pid, tid; bool is_thread, valid; } ht_identity_t;
typedef struct {
    const os64_monitor_snapshot_t *sample;
    ht_row_t rows[HT_ROWS];
    size_t count, selected, scroll;
    ht_identity_t identity;
    ht_sort_t sort;
    ht_mode_t mode;
    bool reverse, tree, threads, idle, zombies, paused, quit;
    unsigned cols, lines, table_top, page, core_page, command_offset, detail_line;
    char filter[64], edit[64], notice[160];
    size_t edit_len;
    uint8_t history[HT_HISTORY]; // 255 = unavailable; newest at history_next - 1
    unsigned history_next, history_count, delay_ms;
    uint64_t self, action_pid;
    bool action_kill, confirm_drawn;
    char action_name[OS64_PROC_NAME_MAX];
    const char *command; // main owns this optional full-command allocation
} ht_view_t;

enum { HT_UP = 256, HT_DOWN, HT_LEFT, HT_RIGHT, HT_HOME, HT_END,
       HT_PGUP, HT_PGDN, HT_REFRESH_THREADS, HT_LOAD_DETAILS, HT_SEND_SIGNAL };
typedef struct { unsigned state, length, number; int pending; } ht_input_t;

void ht_init(ht_view_t *v, uint64_t self);
void ht_layout(ht_view_t *v, unsigned cols, unsigned lines);
void ht_rebuild(ht_view_t *v, const os64_monitor_snapshot_t *sample, bool history);
int ht_key(ht_view_t *v, int key);
int ht_decode(ht_input_t *in, unsigned char byte);
int ht_escape_timeout(ht_input_t *in);
void ht_draw(ht_view_t *v, ht_cell_t *grid);
const char *ht_style(unsigned style);
const os64_monitor_task_t *ht_selected_task(const ht_view_t *v);
// Revalidate the pinned task immediately before one ctl write. Never retry
// a short command write as a second command. Returns 0 on success, -1 otherwise.
int ht_send_signal(const ht_view_t *v);
#endif
