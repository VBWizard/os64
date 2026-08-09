#ifndef OS64_PS_H
#define OS64_PS_H

#include "os64/os64.h"

#define PS_MAX_TASKS 512
#define PS_MAX_THREADS 512

typedef struct {
    bool all_ttys;
    bool all;
    bool full;
    bool threads;
    bool forest;
    bool zombies;
    bool pid_selected;
    uint64_t pid;
    uint32_t current_tty;
} ps_options_t;

void ps_print(const os64_proc_info_t *tasks, int32_t count,
              const ps_options_t *options);

#endif
