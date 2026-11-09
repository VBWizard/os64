// Production PTY replacement transactions with controllable allocation and
// deterministic interleavings. Privileged irq masking is replaced by a host lock.
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define STRLEN_H
#define timeval kernel_timeval
#define MEMCPY_H
#define _PRINTF_H
#undef __USE_MISC
#define SPINLOCK_H
typedef volatile uint32_t spinlock_t;
static uint64_t spinlock_acquire_irqsave(spinlock_t *p) { while(__sync_lock_test_and_set(p,1)){} return 0; }
static void spinlock_release_irqrestore(spinlock_t *p,uint64_t f) { (void)f;__sync_lock_release(p); }
#include "../kernel/src/tty.c"
static unsigned checks,failures,logs,allocations;
static bool refuse;
static void (*on_allocate)(void);
uint32_t kFrameBufferBackgroundColor;
#define CHECK(x) do { ++checks; if(!(x)){++failures;fprintf(stderr,"pty_budget: line %d: %s\n",__LINE__,#x);} } while(0)
void printd(__uint128_t level,const char *fmt,...)
{ (void)level;if(strstr(fmt,"live-only"))++logs; }
void *kmalloc_try(uint64_t n)
{
    ++allocations;
    void (*hook)(void)=on_allocate;on_allocate=NULL;if(hook)hook();
    return refuse?NULL:calloc(1,n);
}
void *kmalloc(uint64_t n) { return calloc(1,n); }
void kfree(void *p) { free(p); }
pipe_t *pipe_create(void) { abort(); }
void pipe_hold(pipe_t *p) { (void)p;abort(); }
static void destroy(tty_t *t)
{
    if(!t)return;
    pty_grid_free(t->cells,t->history_bytes);free(t);kPtyList=NULL;
}
static tty_t *shrinking,*growing;
static void during_shrink(void)
{
    CHECK(s_pty_history_bytes>PTY_HISTORY_BUDGET);
    // The same old charge cannot fund two simultaneous replacement rings.
    CHECK(tty_pty_history(shrinking,1)==OS64_PTY_ERR_BUSY);
    // An over-cap temporary count must not underflow the remaining budget.
    CHECK(tty_pty_history(growing,100)==OS64_PTY_ERR_HISTORY_BUDGET);
    tty_t *fresh=pty_create_slave(8,4,PTY_MODE_GRID);
    CHECK(fresh && fresh->total_lines==fresh->rows);
    if(fresh)CHECK(tty_resize(fresh,10,6)==1);
    destroy(fresh);
}
int main(void)
{
    // Other terminals own the rest of the quota; no host RAM needs to be
    // allocated to model their charge at the reservation boundary.
    s_pty_history_bytes=PTY_HISTORY_BUDGET;
    tty_t *t=pty_create_slave(8,4,PTY_MODE_GRID);
    CHECK(t && t->history_configured && t->history_limit==0);
    CHECK(logs==1);
    if(t){
        CHECK(tty_resize(t,10,6)==1 && t->total_lines==6 && !t->history_bytes);
        CHECK(tty_pty_history(t,1)==OS64_PTY_ERR_HISTORY_BUDGET);
        destroy(t);
    }
    CHECK(s_pty_history_bytes==PTY_HISTORY_BUDGET);
    s_pty_history_bytes=0;
    shrinking=pty_create_slave(8,4,PTY_MODE_GRID);
    growing=pty_create_slave(8,4,PTY_MODE_GRID);
    if(!shrinking || !growing)abort();
    CHECK(tty_pty_history(shrinking,20)==1);
    shrinking->cells[0].ch='A';
    size_t old=shrinking->history_bytes;
    s_pty_history_bytes=PTY_HISTORY_BUDGET;
    // Reducing retention must not need additional quota headroom.
    CHECK(tty_pty_history(shrinking,10)==1);
    CHECK(shrinking->cells[0].ch=='A');
    CHECK(s_pty_history_bytes==PTY_HISTORY_BUDGET-old+shrinking->history_bytes);
    s_pty_history_bytes=PTY_HISTORY_BUDGET;
    old=shrinking->history_bytes;
    on_allocate=during_shrink;
    CHECK(tty_pty_history(shrinking,5)==1);
    CHECK(on_allocate==NULL);
    CHECK(s_pty_history_bytes==PTY_HISTORY_BUDGET-old+shrinking->history_bytes);
    // Allocation failure rolls back its charge and replacement ownership.
    s_pty_history_bytes=PTY_HISTORY_BUDGET;
    tty_cell_t *before=shrinking->cells;uint64_t gen=shrinking->generation;
    refuse=true;
    CHECK(tty_pty_history(shrinking,2)==OS64_PTY_ERR_NO_MEMORY);
    refuse=false;
    CHECK(shrinking->cells==before && shrinking->generation==gen);
    CHECK(s_pty_history_bytes==PTY_HISTORY_BUDGET);
    CHECK(tty_pty_history(shrinking,2)==1);
    size_t phantom=s_pty_history_bytes-shrinking->history_bytes-growing->history_bytes;
    destroy(shrinking);destroy(growing);
    CHECK(s_pty_history_bytes==phantom);
    printf("pty_budget: %u checks, %u failures, %u allocations\n",checks,failures,allocations);
    return failures?1:0;
}
