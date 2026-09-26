#include "os64/work.h"
#include "os64/lock.h"
#include "os64/io.h"
#include "os64/mem.h"
#include "os64/thread.h"
#include "os64/gui.h"

// A submitted slot reserves one job-pipe byte. Reads retain their slots
// until claim; cancellation does not recycle a byte's backing slot.
_Static_assert(OS64_WORK_MAX_JOBS <= 65536, "job table must fit the kernel pipe");
_Static_assert(OS64_WORK_MAX_WORKERS <= 65536, "space credits must fit the kernel pipe");
_Static_assert(OS64_WORK_MAX_JOBS == 256, "ids reserve eight bits for slot index");

enum job_state { FREE, QUEUED, WAITING, RUNNING, DONE, RELEASING };
typedef struct {
    os64_work_t work;
    os64_work_id_t id;
    uint64_t completed;
    void *product;
    int64_t verdict;
    enum job_state state;
    bool cancelled;
    bool charged;
} work_slot_t;

struct os64_work_pool {
    os64_lock_t lock;
    work_slot_t slots[OS64_WORK_MAX_JOBS];
    int32_t threads[OS64_WORK_MAX_WORKERS];
    int32_t started;
    int32_t jobs[2], space[2];
    size_t cap, charged;
    uint64_t serial, completion;
    int64_t window, error;
    uint32_t bit, waiting, credits, finished;
    bool stopping;
};

static bool is_cancelled(void *ctx)
{
    work_slot_t *s = ctx;
    return __atomic_load_n(&s->cancelled, __ATOMIC_ACQUIRE);
}

static void cancel_slots_locked(os64_work_pool_t *p)
{
    for (unsigned i = 0; i < OS64_WORK_MAX_JOBS; ++i)
        if (p->slots[i].state != FREE)
            __atomic_store_n(&p->slots[i].cancelled, true, __ATOMIC_RELEASE);
}

static void fail_pool(os64_work_pool_t *p)
{
    os64_lock_acquire(&p->lock);
    p->error = OS64_WORK_ERR_IO;
    cancel_slots_locked(p);
    os64_lock_release(&p->lock);
    // A caller can observe the error even when failure left no result.
    (void)os64_gui_event_ring(p->window, p->bit);
}

static void ring(os64_work_pool_t *p)
{
    if (os64_gui_event_ring(p->window, p->bit) < 0) {
        os64_lock_acquire(&p->lock);
        p->error = OS64_WORK_ERR_IO;
        cancel_slots_locked(p);
        os64_lock_release(&p->lock);
    }
}

static void notify_space(os64_work_pool_t *p)
{
    os64_lock_acquire(&p->lock);
    unsigned n = !p->stopping && p->waiting > p->credits ? p->waiting - p->credits : 0;
    p->credits += n;
    os64_lock_release(&p->lock);
    // Credits cover writes not started yet, bytes buffered, and successful
    // reads awaiting retirement. Concurrent notifiers cannot reserve twice.
    for (unsigned i = 0; i < n; ++i) {
        const char byte = 1;
        int64_t rc = os64_write(p->space[1], &byte, 1);
        if (rc == 1) continue;
        os64_lock_acquire(&p->lock);
        p->credits -= n - i;
        os64_lock_release(&p->lock);
        if (rc != OS64_INTERRUPTED) fail_pool(p);
        break; // timed predicate checks cover a notification interrupted here
    }
}

// Bounded scans avoid dynamic queue nodes and keep cancellation simple.
// Metadata is under the table lock; no callbacks or I/O run under it.
static work_slot_t *oldest_locked(os64_work_pool_t *p, enum job_state state)
{
    work_slot_t *oldest = NULL;
    for (unsigned i = 0; i < OS64_WORK_MAX_JOBS; ++i) {
        work_slot_t *s = &p->slots[i];
        if (s->state != state) continue;
        uint64_t order = state == DONE ? s->completed : s->id;
        uint64_t previous = !oldest ? UINT64_MAX : state == DONE ? oldest->completed : oldest->id;
        if (!oldest || order < previous) oldest = s;
    }
    return oldest;
}

static bool first_locked(os64_work_pool_t *p, work_slot_t *s)
{
    for (unsigned i = 0; i < OS64_WORK_MAX_JOBS; ++i) {
        work_slot_t *other = &p->slots[i];
        if ((other->state == QUEUED || other->state == WAITING) &&
            other->id < s->id && !is_cancelled(other)) return false;
    }
    return true;
}

// The caller owns a RELEASING slot. Keep its charge until cleanup actually
// frees the objects; neither a new submit nor admission may reuse it early.
static void release_slot(os64_work_pool_t *p, work_slot_t *s)
{
    s->work.release(s->work.job, s->product);
    os64_lock_acquire(&p->lock);
    if (s->charged) p->charged -= s->work.reserve;
    s->charged = false;
    s->state = FREE;
    os64_lock_release(&p->lock);
    notify_space(p);
    // A cancelled job also frees table space for a previously refused submit.
    ring(p);
}

static bool admit(os64_work_pool_t *p, work_slot_t *s)
{
    for (;;) {
        os64_lock_acquire(&p->lock);
        if (is_cancelled(s) || p->stopping || p->error) {
            s->state = RELEASING;
            os64_lock_release(&p->lock);
            notify_space(p);
            return false;
        }
        if (first_locked(p, s) && s->work.reserve <= p->cap - p->charged) {
            p->charged += s->work.reserve;
            s->charged = true;
            s->state = RUNNING;
            os64_lock_release(&p->lock);
            notify_space(p); // younger jobs may now enter even without a release
            return true;
        }
        ++p->waiting;
        os64_lock_release(&p->lock);
        char byte;
        int64_t rc = os64_read_for(p->space[0], &byte, 1, 100);
        os64_lock_acquire(&p->lock);
        --p->waiting;
        if (rc == 1) --p->credits;
        os64_lock_release(&p->lock);
        // Timeout/interruption did not consume a byte; retain its credit.
        if (rc != 1 && rc != OS64_ERR_TIMEOUT && rc != OS64_INTERRUPTED)
            fail_pool(p);
    }
}

static int64_t worker_loop(void *arg)
{
    os64_work_pool_t *p = arg;
    for (;;) {
        char byte;
        int64_t rc = os64_read(p->jobs[0], &byte, 1);
        if (rc == OS64_INTERRUPTED) continue;
        if (rc == 0) return 0;
        if (rc != 1) { fail_pool(p); return -1; }
        os64_lock_acquire(&p->lock);
        work_slot_t *s = oldest_locked(p, QUEUED);
        if (s) s->state = WAITING;
        os64_lock_release(&p->lock);
        if (!s) { fail_pool(p); return -1; }
        if (!admit(p, s)) { release_slot(p, s); continue; }

        void *product = NULL;
        int64_t verdict = s->work.run(s->work.job, is_cancelled, s, &product);
        os64_lock_acquire(&p->lock);
        s->product = product;
        s->verdict = verdict;
        bool discard = is_cancelled(s) || p->stopping || p->error;
        if (discard) s->state = RELEASING;
        else {
            s->completed = ++p->completion;
            s->state = DONE;
        }
        os64_lock_release(&p->lock);
        if (discard) release_slot(p, s);
        else ring(p); // DONE transferred slot access to the owner; use only p
    }
}

static int64_t worker(void *arg)
{
    os64_work_pool_t *p = arg;
    int64_t result = worker_loop(arg);
    // Last access to pool storage. This also protects teardown if an
    // unexpected join-handle error prevents collecting a thread result.
    __atomic_add_fetch(&p->finished, 1u, __ATOMIC_RELEASE);
    return result;
}

os64_work_pool_t *os64_work_pool_create(int32_t workers, size_t reserve_cap,
                                        int64_t window, uint32_t bit)
{
    if (workers < 1 || workers > OS64_WORK_MAX_WORKERS || !bit || (bit & (bit - 1))) return NULL;
    os64_gui_window_state_t state;
    if (os64_gui_window_get_state(window, &state) < 0) return NULL;
    os64_work_pool_t *p = os64_calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->jobs[0] = p->jobs[1] = p->space[0] = p->space[1] = -1;
    p->cap = reserve_cap; p->window = window; p->bit = bit;
    if (os64_pipe(p->jobs) < 0 || os64_pipe(p->space) < 0) goto fail;
    for (int32_t i = 0; i < workers; ++i) {
        int64_t h = os64_thread(worker, p);
        if (h < 0) goto fail;
        p->threads[p->started++] = (int32_t)h;
    }
    return p;
fail:
    os64_work_pool_destroy(p);
    return NULL;
}

os64_work_id_t os64_work_submit(os64_work_pool_t *p, const os64_work_t *work)
{
    if (!p || !work || !work->run || !work->release || work->reserve > p->cap) return 0;
    os64_lock_acquire(&p->lock);
    work_slot_t *s = NULL;
    if (!p->stopping && !p->error && p->serial < (UINT64_MAX >> 8)) {
        for (unsigned i = 0; i < OS64_WORK_MAX_JOBS; ++i) {
            if (p->slots[i].state != FREE) continue;
            s = &p->slots[i];
            *s = (work_slot_t){.work = *work, .state = QUEUED,
                .id = (++p->serial << 8) | i};
            break;
        }
    }
    os64_work_id_t id = s ? s->id : 0;
    os64_lock_release(&p->lock);
    if (!s) return 0;
    const char byte = 1;
    int64_t rc = os64_write(p->jobs[1], &byte, 1);
    if (rc == 1) return id;
    // The sole submitter's unpublished tail cannot be claimed by an older
    // byte. Returning zero leaves the input with the caller, without release.
    os64_lock_acquire(&p->lock);
    s->state = FREE;
    os64_lock_release(&p->lock);
    if (rc != OS64_INTERRUPTED) fail_pool(p);
    notify_space(p);
    return 0;
}

void os64_work_cancel(os64_work_pool_t *p, os64_work_id_t id)
{
    if (!p || !id) return;
    os64_lock_acquire(&p->lock);
    work_slot_t *s = &p->slots[id & 255u];
    if (s->state == FREE || s->id != id || is_cancelled(s)) {
        os64_lock_release(&p->lock);
        return;
    }
    __atomic_store_n(&s->cancelled, true, __ATOMIC_RELEASE);
    bool release = s->state == DONE;
    if (release) s->state = RELEASING;
    os64_lock_release(&p->lock);
    if (release) release_slot(p, s);
    else notify_space(p);
}

bool os64_work_reap(os64_work_pool_t *p, os64_work_id_t *id, int64_t *verdict,
                     void **job, void **product)
{
    if (!p || !id || !verdict || !job || !product) return false;
    os64_lock_acquire(&p->lock);
    work_slot_t *s = oldest_locked(p, DONE);
    if (!s || p->error) { os64_lock_release(&p->lock); return false; }
    *id = s->id; *verdict = s->verdict; *job = s->work.job; *product = s->product;
    if (s->charged) p->charged -= s->work.reserve;
    s->charged = false;
    s->state = FREE;
    os64_lock_release(&p->lock);
    notify_space(p);
    return true;
}

int64_t os64_work_pool_error(os64_work_pool_t *p)
{
    if (!p) return OS64_WORK_ERR_IO;
    os64_lock_acquire(&p->lock);
    int64_t error = p->error;
    os64_lock_release(&p->lock);
    return error;
}

void os64_work_pool_destroy(os64_work_pool_t *p)
{
    if (!p) return;
    os64_lock_acquire(&p->lock);
    p->stopping = true;
    cancel_slots_locked(p);
    os64_lock_release(&p->lock);
    if (p->jobs[1] >= 0) os64_close(p->jobs[1]);
    // Space writers finish before close. Job readers drain buffered bytes
    // before EOF; space waiters observe stopping on their bounded reads.
    for (int32_t i = 0; i < p->started; ++i) {
        int64_t result;
        int64_t rc;
        do { rc = os64_read(p->threads[i], &result, sizeof(result)); }
        while (rc == OS64_INTERRUPTED);
        if (rc != sizeof(result))
            while (__atomic_load_n(&p->finished, __ATOMIC_ACQUIRE) != (uint32_t)p->started)
                os64_yield();
        os64_close(p->threads[i]);
    }
    for (unsigned i = 0; i < OS64_WORK_MAX_JOBS; ++i)
        if (p->slots[i].state != FREE)
            p->slots[i].work.release(p->slots[i].work.job, p->slots[i].product);
    if (p->jobs[0] >= 0) os64_close(p->jobs[0]);
    if (p->space[0] >= 0) os64_close(p->space[0]);
    if (p->space[1] >= 0) os64_close(p->space[1]);
    os64_free(p);
}
