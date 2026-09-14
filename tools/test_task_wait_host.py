#!/usr/bin/env python3
"""Drive production wait across exit publication and dead-child enqueue."""
from pathlib import Path
import re
import subprocess
import tempfile
import sys

root = Path(__file__).resolve().parents[1]
source = Path(sys.argv[1]).read_text() if len(sys.argv) > 1 else (root / 'kernel/src/task.c').read_text()

def function(name):
    m = re.search(r'(?:static )?(?:bool|uint64_t|task_t \*|task_t\*)\s*' + name + r'\([^;]+?\)\s*\{', source)
    assert m, name
    end, depth = m.end(), 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[m.start():end]

program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#define NO_TASK NULL
#define TASK_WAIT_INTERRUPTED UINT64_MAX
#define TASK_WAIT_BACKSTOP_TICKS 1
#define SIGSLEEP 1
#define printd(...) ((void)0)
typedef struct thread {int unused;} thread_t;
typedef struct task {
 struct task *next,*parentTask,*deadChildHead,*deadChildNext;
 thread_t *threads,*waitThread;
 bool exited,retValCollected,controllingShell,waitingForChild;
 uint64_t taskID,retVal;
} task_t;
typedef struct tty {unsigned fg_lock;task_t *fgTask;} tty_t;
typedef struct {task_t *task;thread_t *currentThread;} core_local_storage_t;
static task_t parent,child,*kTaskList;
static thread_t thread;
static core_local_storage_t cls;
static unsigned kDeadChildLock,unlocks,parks,mode;
static uint64_t kTicksSinceStart;
static core_local_storage_t *get_core_local_storage(void) {return &cls;}
static uint64_t spinlock_acquire_irqsave(unsigned *l) {assert(!*l);*l=1;return 0;}
static void spinlock_release_irqrestore(unsigned *l,uint64_t f) {
 (void)f;assert(*l);*l=0;
 if(l==&kDeadChildLock && ++unlocks==1 && mode==1) {
  child.exited=true;parent.deadChildHead=&child;
 }
}
static tty_t *task_tty_hold(task_t *t) {(void)t;return NULL;}
static void task_tty_release(tty_t *t) {assert(!t);}
static bool signal_park_must_end(thread_t *t) {(void)t;return false;}
static void signal_raise(int sig,uint64_t tick,thread_t *t) {
 (void)tick;assert(sig==SIGSLEEP && t==&thread && ++parks<3);
 child.exited=true;parent.deadChildHead=&child;
}
'''
for name in ('task_find_dead_child', 'task_find_live_child', 'task_is_live_child',
             'task_has_uncollected_child_locked', 'task_wait'):
    if name in source:
        program += function(name) + '\n'
program += r'''
int main(void) {
 for(mode=0;mode<4;mode++) {
  parent=(task_t){.threads=&thread};
  child=(task_t){.parentTask=&parent,.taskID=42,.retVal=314,.exited=mode!=1};
  parent.next=mode==2?NULL:&child;kTaskList=&parent;
  child.retValCollected=mode==3;cls=(core_local_storage_t){&parent,&thread};
  unlocks=parks=0;kDeadChildLock=0;
  uint64_t code=0,pid=task_wait(&parent,42,&code);
  if(mode<2) assert(pid==42 && code==314 && child.retValCollected);
  else assert(pid==0 && code==0 && !parks);
 }
 puts("task wait: exit-before-enqueue and exit-between-probes retain child; missing and collected children refused PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='task-wait-') as directory:
    work = Path(directory)
    (work / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    str(work / 'test.c'), '-o', str(work / 'test')], check=True)
    subprocess.run([str(work / 'test')], check=True)
