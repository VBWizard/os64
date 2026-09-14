#!/usr/bin/env python3
"""Run the actual task hold/release and two-phase reaper with controlled corpses."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'kernel/src/task.c').read_text()


def function(name):
    m = re.search(r'(?:static )?(?:void|int) ' + name + r'\([^;]+?\)\s*\{', source)
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
#include <stdlib.h>
#include <stdio.h>
#define NO_TASK NULL
#define THREAD_STATE_ZOMBIE 1
#define THREAD_STATE_NONE 0
#define printd(...) ((void)0)
typedef struct thread {bool exited;int threadState;struct thread *taskNext;} thread_t;
typedef struct task {
 struct task *next,*burialNext,*parentTask,*deadChildHead,*deadChildNext;
 bool exited,retValCollected,autoReap;
 uint32_t lifetimeHolds;
 thread_t *threads;
} task_t;
static task_t *kTaskList,*kBurialList;
static unsigned kDeadChildLock,freed;
static uint64_t spinlock_acquire_irqsave(unsigned *l) {assert(!*l);*l=1;return 0;}
static void spinlock_release_irqrestore(unsigned *l,uint64_t f) {(void)f;assert(*l);*l=0;}
static bool task_threads_all_retired(task_t *t) {(void)t;return true;}
static void task_destroy(task_t *t) {assert(!t->lifetimeHolds);freed++;free(t);}
static void task_remove_dead_child_locked(task_t *p,task_t *c) {
 assert(kDeadChildLock);task_t **link=&p->deadChildHead;
 while(*link!=c) {assert(*link);link=&(*link)->deadChildNext;}
 *link=c->deadChildNext;
}
static void task_reparent_orphans_locked(task_t *t) {(void)t;assert(kDeadChildLock);}
static void scheduler_remove_task(task_t *t) {
 task_t **link=&kTaskList;
 while(*link!=t) {assert(*link);link=&(*link)->next;}
 *link=t->next;
}
'''
program += '\n'.join(function(n) for n in ('task_hold', 'task_release', 'task_reap_eligible_zombies'))
program += r'''
int main(void) {
 for(unsigned mode=0;mode<3;mode++) {
  task_t parent={0};task_t *held=calloc(1,sizeof(*held)),*other=calloc(1,sizeof(*other));
  assert(held && other);freed=0;kBurialList=NULL;kTaskList=&parent;
  parent.next=parent.deadChildHead=held;held->next=held->deadChildNext=other;
  held->parentTask=other->parentTask=&parent;
  held->exited=other->exited=true;other->autoReap=true;
  held->retValCollected=mode==0;held->autoReap=mode==1;parent.exited=mode==2;
  task_hold(held);task_hold(held);
  assert(task_reap_eligible_zombies(32)==1 && parent.deadChildHead==held);
  assert(task_reap_eligible_zombies(32)==1 && freed==1);
  for(unsigned pass=0;pass<8;pass++) assert(task_reap_eligible_zombies(32)==0);
  assert(held->lifetimeHolds==2 && parent.next==held);
  task_release(held);assert(task_reap_eligible_zombies(32)==0);
  task_release(held);
  assert(task_reap_eligible_zombies(32)==1 && !parent.deadChildHead && freed==1);
  assert(task_reap_eligible_zombies(32)==1 && freed==2 && !kBurialList);
 }
 puts("task holds: collected, autoreaped and orphaned corpses survive repeated passes; other burials proceed; final release restores burial PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='task-hold-') as directory:
    work = Path(directory)
    (work / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    str(work / 'test.c'), '-o', str(work / 'test')], check=True)
    subprocess.run([str(work / 'test')], check=True)
