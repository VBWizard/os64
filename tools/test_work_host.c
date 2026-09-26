// Actual pool code over host pipes/threads. Internal state is inspected only
// with the table lock held, or with workers stopped at an explicit barrier.
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include "../userland/libos64/work.c"

#define LOAD(p) __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define STORE(p,v) __atomic_store_n((p),(v),__ATOMIC_RELEASE)
#define ADD(p,v) __atomic_add_fetch((p),(v),__ATOMIC_ACQ_REL)
typedef struct { pthread_t thread; int64_t (*fn)(void *); void *arg; int64_t result; bool used; } host_thread;
static host_thread threads[128];
static int live_allocs, live_handles, rings, fail_thread = -1, fail_pipe = -1;
static int fail_write_fd = -1, fail_write_rc, fail_read_fd = -1, fail_read_rc;
static int read_faults;
static int pause_write_fd = -1, pause_read_fd = -1, write_paused, read_paused;

static void until(int *p, int n)
{
    for (int i = 0; LOAD(p) < n && i < 10000; ++i) usleep(1000);
    assert(LOAD(p) >= n);
}
void os64_yield(void) { sched_yield(); }
void *os64_calloc(size_t n, size_t s) { void *p = calloc(n,s); if (p) ADD(&live_allocs,1); return p; }
void os64_free(void *p) { if (p) ADD(&live_allocs,-1); free(p); }
int64_t os64_gui_window_get_state(int64_t h, os64_gui_window_state_t *s)
{ (void)s; return h == 77 ? 0 : -1; }
int64_t os64_gui_event_ring(int64_t h, uint32_t bit)
{ assert(h == 77 && bit == 4); ADD(&rings,1); return 0; }
int64_t os64_pipe(int32_t fds[2])
{
    if (fail_pipe == 0) return -1;
    if (fail_pipe > 0) --fail_pipe;
    if (pipe(fds)) return -1;
    assert(fcntl(fds[0],F_SETFL,O_NONBLOCK)==0);
    ADD(&live_handles,2); return 0;
}
static void *host_start(void *v)
{ host_thread *t = v; t->result = t->fn(t->arg); return NULL; }
int64_t os64_thread(int64_t (*fn)(void *), void *arg)
{
    if (fail_thread == 0) return -1;
    if (fail_thread > 0) --fail_thread;
    for (unsigned i = 0; i < 128; ++i) if (!threads[i].used) {
        threads[i] = (host_thread){.fn=fn,.arg=arg,.used=true};
        assert(!pthread_create(&threads[i].thread,NULL,host_start,&threads[i]));
        ADD(&live_handles,1); return 10000+i;
    }
    abort();
}
int64_t os64_close(int32_t h)
{
    if (h >= 10000) threads[h-10000].used = false;
    else assert(!close(h));
    ADD(&live_handles,-1); return 0;
}
int64_t os64_write(int32_t fd, const void *b, size_t n)
{
    if (fd == LOAD(&pause_write_fd)) {
        STORE(&write_paused,1);
        while (fd == LOAD(&pause_write_fd)) usleep(100);
    }
    if (fd == LOAD(&fail_write_fd)) return LOAD(&fail_write_rc);
    ssize_t r = write(fd,b,n); return r < 0 ? OS64_INTERRUPTED : r;
}
int64_t os64_read_for(int32_t fd, void *b, size_t n, uint64_t ms)
{
    if (fd == LOAD(&fail_read_fd)) {
        int result=LOAD(&fail_read_rc);
        STORE(&fail_read_fd,-1); ADD(&read_faults,1); return result;
    }
    struct pollfd f = {.fd=fd,.events=POLLIN};
    int rc = poll(&f,1,ms == OS64_WAIT_FOREVER ? -1 : (int)ms);
    if (!rc) return OS64_ERR_TIMEOUT;
    if (rc < 0) return OS64_INTERRUPTED;
    // Multiple readers may consume the readiness first. Real kernel reads
    // are atomic; retry stale readiness in the fake using nonblocking read fds.
    ssize_t r = read(fd,b,n);
    if (r < 0) return errno == EAGAIN ? OS64_ERR_TIMEOUT : OS64_INTERRUPTED;
    if (r == 1 && fd == LOAD(&pause_read_fd)) {
        STORE(&read_paused,1);
        while (fd == LOAD(&pause_read_fd)) usleep(100);
    }
    return r;
}
int64_t os64_read(int32_t h, void *b, size_t n)
{
    if (h >= 10000) {
        host_thread *t = &threads[h-10000];
        assert(n == 8 && !pthread_join(t->thread,NULL));
        memcpy(b,&t->result,8); return 8;
    }
    for (;;) {
        int64_t r = os64_read_for(h,b,n,OS64_WAIT_FOREVER);
        if (r != OS64_ERR_TIMEOUT) return r;
    }
}

typedef struct { int number, started, released, gate, cancel_seen; bool wait; } job;
static int64_t run(void *v, bool (*cancel)(void *), void *ctx, void **out)
{
    job *j = v; ADD(&j->started,1);
    while (j->wait && !LOAD(&j->gate) && !cancel(ctx)) usleep(100);
    if (cancel(ctx)) STORE(&j->cancel_seen,1);
    int *p = malloc(sizeof(*p)); assert(p); *p = j->number * 3; *out = p;
    return j->number;
}
static void release(void *v, void *p) { job *j=v; ADD(&j->released,1); free(p); }
static os64_work_id_t submit(os64_work_pool_t *p, job *j, size_t reserve)
{ os64_work_t w={run,release,j,reserve}; return os64_work_submit(p,&w); }
static os64_work_pool_t *pool(int workers, size_t cap)
{ os64_work_pool_t *p=os64_work_pool_create(workers,cap,77,4); assert(p); return p; }
static os64_work_id_t collect(os64_work_pool_t *p, job *expected)
{
    os64_work_id_t id; int64_t verdict; void *j,*product;
    for (int i=0;i<10000;++i) {
        if (os64_work_reap(p,&id,&verdict,&j,&product)) {
            assert(j==expected && verdict==expected->number && *(int*)product==verdict*3);
            release(j,product); return id;
        }
        usleep(1000);
    }
    abort();
}
static void state(os64_work_pool_t *p, os64_work_id_t id, enum job_state wanted)
{
    for (int i=0;i<10000;++i) {
        os64_lock_acquire(&p->lock);
        bool yes=p->slots[id&255].state==wanted;
        os64_lock_release(&p->lock);
        if (yes) return;
        usleep(1000);
    }
    abort();
}
static void clean(void) { assert(!LOAD(&live_handles) && !LOAD(&live_allocs)); }

static void lifecycle(void)
{
    assert(!os64_work_pool_create(0,10,77,4));
    assert(!os64_work_pool_create(33,10,77,4));
    assert(!os64_work_pool_create(1,10,77,3));
    assert(!os64_work_pool_create(1,10,66,4));
    for (int i=0;i<2;++i) { fail_pipe=i; assert(!os64_work_pool_create(4,10,77,4)); clean(); }
    fail_pipe=-1;
    for (int i=0;i<4;++i) { fail_thread=i; assert(!os64_work_pool_create(4,10,77,4)); clean(); }
    fail_thread=-1;
    os64_work_pool_t *p=pool(4,40);
    job j[20]={0}; os64_work_id_t ids[20];
    for (int i=0;i<20;++i) { j[i].number=i; j[i].wait=true; ids[i]=submit(p,&j[i],10); assert(ids[i]); }
    // Deliberately complete the four admitted jobs in reverse order.
    for (int i=0;i<4;++i) until(&j[i].started,1);
    for (int i=3;i>=0;--i) { STORE(&j[i].gate,1); assert(collect(p,&j[i])==ids[i]); }
    for (int i=4;i<20;++i) { until(&j[i].started,1); STORE(&j[i].gate,1); assert(collect(p,&j[i])==ids[i]); }
    os64_work_pool_destroy(p);
    for (int i=0;i<20;++i) assert(j[i].released==1);
    clean(); puts("work: completion order, products, partial creation PASS");
}

static void ownership(void)
{
    os64_work_pool_t *p=pool(4,10); job j[5]={0};
    os64_work_id_t a=submit(p,&j[0],10); state(p,a,DONE);
    os64_work_id_t b=submit(p,&j[1],10), c=submit(p,&j[2],1);
    state(p,b,WAITING); state(p,c,WAITING); usleep(150000);
    assert(!LOAD(&j[1].started) && !LOAD(&j[2].started));
    os64_work_cancel(p,a); until(&j[0].released,1); state(p,b,DONE);
    assert(!LOAD(&j[2].started)); // finished B retains its full reservation
    assert(collect(p,&j[1])==b); assert(collect(p,&j[2])==c);
    j[3].wait=true; os64_work_id_t d=submit(p,&j[3],10); until(&j[3].started,1);
    os64_work_cancel(p,a); // slot was reused; stale generation cannot cancel D
    usleep(20000); assert(!LOAD(&j[3].cancel_seen));
    os64_work_id_t e=submit(p,&j[4],10); state(p,e,WAITING);
    os64_work_cancel(p,e); until(&j[4].released,1); assert(!j[4].started);
    os64_work_cancel(p,d); until(&j[3].released,1); assert(j[3].cancel_seen);
    os64_work_pool_destroy(p); clean(); puts("work: cap through DONE, FIFO, stale ids, cancellation PASS");
}

static void *admit_younger(void *arg)
{
    os64_work_pool_t *p=arg;
    assert(admit(p,&p->slots[1]));
    return NULL;
}
static void claimed_order(void)
{
    // Model two workers immediately after claim, with the older paused
    // before entering admission. Execute the production admission routine.
    os64_work_pool_t *p=pool(1,10); job jobs[2]={0};
    os64_lock_acquire(&p->lock);
    p->slots[0]=(work_slot_t){.work={run,release,&jobs[0],8},.id=256,.state=WAITING};
    p->slots[1]=(work_slot_t){.work={run,release,&jobs[1],1},.id=513,.state=WAITING};
    os64_lock_release(&p->lock);
    pthread_t t; assert(!pthread_create(&t,NULL,admit_younger,p));
    usleep(150000);
    os64_lock_acquire(&p->lock);
    assert(p->charged==0 && p->slots[1].state==WAITING);
    os64_lock_release(&p->lock);
    assert(admit(p,&p->slots[0])); pthread_join(t,NULL);
    assert(p->charged==9);
    os64_work_pool_destroy(p); clean();
    assert(jobs[0].released==1 && jobs[1].released==1);
    puts("work: claimed older job holds younger admission PASS");
}

static void full_and_destroy(void)
{
    os64_work_pool_t *p=pool(1,10); job j[257]={0};
    j[0].wait=true; assert(submit(p,&j[0],10)); until(&j[0].started,1);
    for (int i=1;i<256;++i) { os64_work_id_t id=submit(p,&j[i],1); assert(id); os64_work_cancel(p,id); }
    assert(!submit(p,&j[256],1)); assert(!j[256].released);
    os64_work_pool_destroy(p);
    for (int i=0;i<256;++i) assert(j[i].released==1 && j[i].started==(i==0));
    clean();
    p=pool(4,4); job active[10]={0};
    for (int i=0;i<10;++i) { active[i].wait=true; assert(submit(p,&active[i],1)); }
    for (int i=0;i<4;++i) until(&active[i].started,1);
    os64_work_pool_destroy(p);
    for (int i=0;i<10;++i) assert(active[i].released==1);
    clean(); puts("work: full table refuses, queued cancellation, destroy ten PASS");
}

static void *notifier(void *p) { notify_space(p); return NULL; }
static void *release_shutdown_writer(void *v)
{
    os64_work_pool_t *p=v;
    until(&write_paused,1);
    for (;;) {
        os64_lock_acquire(&p->lock); bool stopping=p->stopping; os64_lock_release(&p->lock);
        if (stopping) break;
        usleep(100);
    }
    assert(fcntl(p->space[0],F_GETFD)>=0 && fcntl(p->space[1],F_GETFD)>=0);
    STORE(&pause_write_fd,-1);
    return NULL;
}
static void credits(void)
{
    // Isolate the notification algorithm without scheduling a worker. A
    // paused writer models P; a consumed unretired byte models R.
    os64_work_pool_t *p=pool(1,10);
    os64_lock_acquire(&p->lock); p->waiting=1; os64_lock_release(&p->lock);
    STORE(&pause_write_fd,p->space[1]); STORE(&write_paused,0);
    pthread_t t; assert(!pthread_create(&t,NULL,notifier,p)); until(&write_paused,1);
    for (int i=0;i<70000;++i) notify_space(p);
    assert(p->credits==1);
    char b; assert(os64_read_for(p->space[0],&b,1,1)==OS64_ERR_TIMEOUT);
    assert(p->credits==1); STORE(&pause_write_fd,-1); pthread_join(t,NULL);
    assert(os64_read_for(p->space[0],&b,1,1)==1);
    for (int i=0;i<70000;++i) notify_space(p);
    assert(p->credits==1); // read has not retired its credit
    os64_lock_acquire(&p->lock); --p->credits; os64_lock_release(&p->lock);
    STORE(&fail_write_rc,OS64_INTERRUPTED); STORE(&fail_write_fd,p->space[1]);
    notify_space(p); assert(p->credits==0 && !os64_work_pool_error(p));
    STORE(&fail_write_rc,-99); notify_space(p);
    assert(p->credits==0 && os64_work_pool_error(p)==OS64_WORK_ERR_IO);
    STORE(&fail_write_fd,-1);
    os64_lock_acquire(&p->lock); p->waiting=0; os64_lock_release(&p->lock);
    os64_work_pool_destroy(p); clean(); puts("work: 140000 notifications, P/R credits, timeout, failed writes PASS");

    // Hold a worker's accounted notification inside write while teardown
    // joins it. A controller releases the syscall after checking both ends.
    p=pool(1,10); job j={0};
    os64_lock_acquire(&p->lock); p->waiting=1; os64_lock_release(&p->lock);
    STORE(&write_paused,0); STORE(&pause_write_fd,p->space[1]);
    assert(submit(p,&j,1)); until(&write_paused,1);
    assert(!pthread_create(&t,NULL,release_shutdown_writer,p));
    os64_work_pool_destroy(p); pthread_join(t,NULL);
    assert(j.released==1); clean();
    puts("work: destroy retains space handles through in-flight writer PASS");
}

static void *check_unpublished(void *v)
{
    os64_work_pool_t *p=v; until(&write_paused,1);
    os64_lock_acquire(&p->lock);
    unsigned queued=0;
    for (unsigned i=0;i<OS64_WORK_MAX_JOBS;++i) queued+=p->slots[i].state==QUEUED;
    assert(queued==1);
    os64_lock_release(&p->lock);
    char b; assert(os64_read_for(p->jobs[0],&b,1,0)==OS64_ERR_TIMEOUT);
    STORE(&pause_write_fd,-1); return NULL;
}
static void job_handoffs(void)
{
    os64_work_pool_t *p=pool(1,1); job j={0};
    STORE(&pause_write_fd,p->jobs[1]); STORE(&write_paused,0);
    pthread_t monitor; assert(!pthread_create(&monitor,NULL,check_unpublished,p));
    os64_work_id_t first=submit(p,&j,1); assert(first);
    pthread_join(monitor,NULL); assert(collect(p,&j)==first);
    j=(job){0};
    STORE(&fail_write_fd,p->jobs[1]); STORE(&fail_write_rc,OS64_INTERRUPTED);
    assert(!submit(p,&j,1)); assert(!j.started && !j.released);
    STORE(&fail_write_fd,-1);
    STORE(&pause_read_fd,p->jobs[0]); STORE(&read_paused,0);
    os64_work_id_t id=submit(p,&j,1); assert(id); until(&read_paused,1);
    os64_work_cancel(p,id); state(p,id,QUEUED);
    assert(!j.started && !j.released);
    STORE(&pause_read_fd,-1); until(&j.released,1); assert(!j.started);
    for (int i=0;i<1000;++i) {
        job k={0}; id=submit(p,&k,1); assert(id); os64_work_cancel(p,id);
        until(&k.released,1); state(p,id,FREE);
    }
    os64_work_pool_destroy(p); clean(); puts("work: submit rollback, read-before-claim, 1000 cancel/reuse PASS");

    p=pool(1,1); j=(job){0};
    STORE(&fail_write_fd,p->jobs[1]); STORE(&fail_write_rc,-99);
    assert(!submit(p,&j,1)); assert(os64_work_pool_error(p)==OS64_WORK_ERR_IO);
    STORE(&fail_write_fd,-1); os64_work_pool_destroy(p);
    assert(!j.started && !j.released); clean();
    puts("work: permanent publication failure retains caller ownership PASS");
}

static void read_failures(void)
{
    for (unsigned permanent=0;permanent<2;++permanent) {
        os64_work_pool_t *p=pool(2,1); job j[2]={0};
        os64_work_id_t a=submit(p,&j[0],1); state(p,a,DONE);
        STORE(&read_faults,0);
        STORE(&fail_read_rc,permanent?-99:OS64_INTERRUPTED);
        STORE(&fail_read_fd,p->space[0]);
        os64_work_id_t b=submit(p,&j[1],1); assert(b); until(&read_faults,1);
        if (permanent) {
            until(&j[1].released,1); assert(!j[1].started);
            assert(os64_work_pool_error(p)==OS64_WORK_ERR_IO);
        } else {
            assert(!os64_work_pool_error(p));
            assert(collect(p,&j[0])==a); assert(collect(p,&j[1])==b);
        }
        os64_work_pool_destroy(p); clean();
        assert(j[0].released==1 && j[1].released==1);
    }
    puts("work: interrupted read retries; permanent read failure cancels safely PASS");
}

int main(void)
{
    alarm(40);
    lifecycle(); ownership(); claimed_order(); full_and_destroy(); credits(); job_handoffs(); read_failures();
    printf("work host: PASS (%d doorbells, zero live allocations/handles)\n",rings);
}
