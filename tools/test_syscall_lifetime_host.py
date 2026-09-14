#!/usr/bin/env python3
"""Compile actual syscall pipe-write and spawn-publication code with lifetime seams.

The write test retains the real dispatch prelude, pipe case and pin wrapper;
unrelated device cases are excluded. The spawn test retains the actual code
from scheduler submission through the returned PID. Stubs control death and
publication timing, and assert references are released before either death.
"""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'kernel/src/syscall.c').read_text()


def function(name):
    match = re.search(r'static (?:uint64_t|void) ' + name + r'\([^;]+?\)\s*\{', source)
    assert match, name
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]


header = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <setjmp.h>
#include "os64/syscall_numbers.h"
#include "os64/signal.h"
#define SIGPIPE 13
#define SIGHUP 1
#define HANDLE_PIPE_WRITE 3
#define HANDLE_NET_TCP 7
#define READ_CHUNK_SIZE 4096
#define PIPE_CAPACITY 8192
#define PIPE_ERR_CLOSED (-1)
#define PIPE_ERR_INTERRUPTED (-3)
#define SYSCALL_RESULT_INVALID ((uint64_t)-1)
#define SYSCALL_RESULT_BAD_USER_DATA ((uint64_t)-2)
typedef struct thread { struct thread *taskNext; struct { unsigned sigind; } signals; } thread_t;
typedef struct { bool is_pty, masterClosed, buried; unsigned holds; } tty_t;
typedef struct { unsigned signalLock; void *sighandler[32]; thread_t *threads; tty_t *tty; uint64_t taskID; } task_t;
typedef struct { int type; void *object; } handle_t;
typedef struct { task_t *task; } core_local_storage_t;
static task_t task;
static thread_t thread;
static core_local_storage_t cls = {&task};
'''

write_stubs = r'''
typedef struct { unsigned writers; } pipe_t;
static pipe_t pipe;
static unsigned pins, allocated, writes, fail_on;
static long answer;
static bool caught, copy_ok = true;
static char scratch[READ_CHUNK_SIZE];
static jmp_buf death;
static core_local_storage_t *get_core_local_storage(void) { return &cls; }
static uint64_t syscall_io_deadline(uint64_t ms) { return ms; }
static bool handle_pin(task_t *t, int fd, handle_t *h) {
 assert(t == &task); if(fd != 3) return false;
 h->type=HANDLE_PIPE_WRITE; h->object=&pipe; pins++; pipe.writers++; return true;
}
static void handle_unpin(handle_t *h) { assert(h->object==&pipe && pins==1); pins--; pipe.writers--; }
static void *syscall_io_scratch(char **p) { *p=scratch; return scratch; }
static void *kmalloc(size_t n) { allocated++; return malloc(n); }
static void kfree(void *p) { assert(allocated); allocated--; free(p); }
static bool copy_user_buffer(const void *p, void *out, size_t n) { if(!copy_ok)return false; memcpy(out,p,n);return true; }
static long pipe_write(pipe_t *p, const char *buf, size_t n) {
 assert(p==&pipe && buf && pins==1); return ++writes==fail_on ? answer : (long)n;
}
static uint64_t spinlock_acquire_irqsave(unsigned *p) { assert(!*p); *p=1; return 0; }
static void spinlock_release_irqrestore(unsigned *p,uint64_t f) { (void)f; assert(*p); *p=0; }
static void sigset_add(unsigned *set,int sig) { *set |= 1u<<sig; }
static bool current_thread_will_catch(void) { return caught; }
static void die_checked(task_t *t,int code) {
 assert(t==&task && !pins && !allocated && !task.signalLock);
 assert(pipe.writers==1); pipe.writers--; longjmp(death,code);
}
static void raise_sigpipe_and_die(task_t *t) { die_checked(t,141); }
static void raise_terminating_signal_and_die(task_t *t,void *th) { assert(!th); die_checked(t,130); }
'''

body = function('syscall_write_pinned')
body = (body[:body.index('switch (h->type)')] + '(void)deadline;\nswitch(h->type) {\n'
        + body[body.index('case HANDLE_PIPE_WRITE:'):body.index('case HANDLE_NET_TCP:')]
        + 'default: return SYSCALL_RESULT_INVALID; }}')
enum = re.search(r'typedef enum\s*\{[^}]+\}\s*write_death_t;', source)
write_program = header + (enum.group(0) if enum else '') + write_stubs + body + function('syscall_write') + r'''
int main(void) {
 char data[PIPE_CAPACITY+1]={0}; task.threads=&thread;
 for(unsigned partial=0;partial<2;partial++) {
  pipe.writers=1; writes=0;fail_on=partial+1;answer=PIPE_ERR_CLOSED;
  int code=setjmp(death);
  if(!code) { syscall_write(3,(uintptr_t)data,partial?sizeof(data):1,OS64_WAIT_FOREVER,0,0);assert(false); }
  assert(code==141 && !pipe.writers && !pins && !allocated);
 }
 pipe.writers=1;task.sighandler[SIGPIPE]=(void *)1;
 writes=0;fail_on=1;answer=PIPE_ERR_CLOSED;
 assert((int64_t)syscall_write(3,(uintptr_t)data,1,OS64_WAIT_FOREVER,0,0)==OS64_INTERRUPTED);
 assert(!pins && pipe.writers==1 && thread.signals.sigind==(1u<<SIGPIPE));
 writes=0;fail_on=2;
 assert(syscall_write(3,(uintptr_t)data,sizeof(data),OS64_WAIT_FOREVER,0,0)==PIPE_CAPACITY);
 assert(!pins && !allocated && pipe.writers==1);
 task.sighandler[SIGPIPE]=NULL;writes=0;fail_on=1;answer=PIPE_ERR_INTERRUPTED;caught=false;
 int code=setjmp(death);
 if(!code) {syscall_write(3,(uintptr_t)data,1,OS64_WAIT_FOREVER,0,0);assert(false);}
 assert(code==130 && !pins && !pipe.writers);
 pipe.writers=1;copy_ok=false;
 assert(syscall_write(3,(uintptr_t)data,sizeof(data),OS64_WAIT_FOREVER,0,0)==SYSCALL_RESULT_BAD_USER_DATA);
 assert(!pins && !allocated && pipe.writers==1);
 puts("write lifetime: default SIGPIPE and termination unpin before death; caught SIGPIPE, partial progress and copy failure PASS");
}
'''

tail = function('spawn_do_create')
tail = tail[tail.index('scheduler_submit_new_task(child);'):]
spawn_program = header + r'''
typedef struct { tty_t *ttySlave; long result; } spawn_params_t;
static tty_t terminal;
static unsigned hangups;
static bool published, close_on_publish;
static void scheduler_submit_new_task(task_t *t) {
 assert(t==&task); if(close_on_publish) terminal.masterClosed=true;
 // The master's earlier sweep cannot see this child until publication.
 published=true;
}
static tty_t *task_tty_hold(task_t *t) {
 assert(published && t==&task);tty_t *tty=t->tty;
 if(!tty || tty->buried)return NULL;
 tty->holds++;return tty;
}
static void task_tty_release(tty_t *t) {assert(t->holds==1);t->holds--;}
static void task_signal_and_nudge(task_t *t,int sig) {assert(published && t==&task && sig==SIGHUP);hangups++;}
static void publication(spawn_params_t *p) { task_t *child=&task;
''' + tail + r'''
int main(void) {
 task.tty=&terminal;task.taskID=42;
 for(unsigned explicit=0;explicit<2;explicit++) {
  for(unsigned closed=0;closed<2;closed++) {
   terminal=(tty_t){.is_pty=true};published=false;hangups=0;close_on_publish=closed;
   spawn_params_t p={.ttySlave=explicit?&terminal:NULL};publication(&p);
   assert(hangups==closed && !terminal.holds && p.result==42);
  }
 }
 terminal=(tty_t){.is_pty=true,.buried=true,.masterClosed=true};close_on_publish=false;hangups=0;
 spawn_params_t p={0};publication(&p);assert(!hangups && !terminal.holds);
 terminal=(tty_t){0};publication(&p);assert(!hangups && !terminal.holds);
 puts("spawn publication: inherited and explicit PTYs see prior hangup; open, buried and VT terminals release holds PASS");
}
'''

with tempfile.TemporaryDirectory(prefix='syscall-lifetime-') as directory:
    work = Path(directory)
    failures = []
    for name, program in (('write', write_program), ('spawn', spawn_program)):
        path = work / (name + '.c')
        path.write_text(program)
        subprocess.run(['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-function', '-Wno-unused-variable', '-Wno-clobbered',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        '-I' + str(root / 'abi/include'), str(path), '-o', str(work / name)], check=True)
        result = subprocess.run([str(work / name)])
        if result.returncode:
            failures.append(name)
    assert not failures, failures
