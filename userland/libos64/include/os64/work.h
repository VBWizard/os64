#ifndef OS64_WORK_H
#define OS64_WORK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct os64_work_pool os64_work_pool_t;
typedef uint64_t os64_work_id_t;

// Fixed storage bounds keep submit allocation-free. Four workers are a
// useful starting point; the caller chooses a positive count at creation.
// A queued job holds only its slot until admission charges its reserve, so
// the table is sized for a page's burst: yonder submits every module a page
// imports at once, 227 on one corpus page (2026-10-31).
#define OS64_WORK_MAX_JOBS 1024
#define OS64_WORK_MAX_WORKERS 32
#define OS64_WORK_ERR_IO (-1)

typedef struct {
    // Runs on a worker. Poll cancelled(ctx) between bounded stages. The
    // predicate and ctx are borrowed until run returns. *out starts NULL.
    int64_t (*run)(void *job, bool (*cancelled)(void *ctx), void *ctx, void **out);
    // Required; releases input and product, including product == NULL when
    // run was skipped. May run on the owner or a worker; no UI operations,
    // pool re-entry or unbounded waits. Reaped objects are caller-owned.
    void (*release)(void *job, void *product);
    void *job;
    // Declared peak callback allocation and retained-result bytes. Charged
    // at admission through reap/release. Pool metadata and pre-existing
    // inputs are outside this budget; callers must bound queued inputs.
    size_t reserve;
} os64_work_t;

// Call public operations on the creating thread, outside signal handlers.
// The window must belong to this task and outlive destroy. doorbell_bit is
// one nonzero bit reserved by the application for this pool. No jobs are
// started on failure; partial creation unwinds. NULL means refusal.
os64_work_pool_t *os64_work_pool_create(int32_t workers, size_t reserve_cap,
                                        int64_t window, uint32_t doorbell_bit);
// The same, naming the workers prefix-01, prefix-02 and on (os64/thread.h's
// os64_thread_named), so a monitor tells them apart. A pool's workers run
// every kind of job, so the prefix says what they are, not what they are
// doing ("worker"). NULL or "" leaves them unnamed. A prefix whose names
// would break the rule (printable ASCII, 63 bytes with the number) refuses
// the pool before any worker starts.
os64_work_pool_t *os64_work_pool_create_named(int32_t workers, size_t reserve_cap,
                                              int64_t window, uint32_t doorbell_bit,
                                              const char *prefix);

// Copies the descriptor. A nonzero id transfers input ownership; zero
// refuses (invalid args, full table, exhausted ids, broken pool or failed
// publication), and the caller retains input. Oversize reserves refuse.
// Ids are pool-local; retain the pool association with each id.
os64_work_id_t os64_work_submit(os64_work_pool_t *, const os64_work_t *);

// Stale ids are ignored. Cancellation surrenders input AND product to the
// pool: run cooperates, release performs cleanup, and reap omits the job.
void os64_work_cancel(os64_work_pool_t *, os64_work_id_t);

// Reap in completion order. Output pointers are required; false leaves
// them untouched. True transfers both objects and drops the reservation.
// Drain on a doorbell, also legal at any time; the job table is the record.
bool os64_work_reap(os64_work_pool_t *, os64_work_id_t *id, int64_t *verdict,
                     void **job, void **product);

// Sticky infrastructure error (zero while healthy). On error, destroy
// the pool: it cancels and releases unreaped jobs. Callback verdicts do
// not set this error. NULL returns OS64_WORK_ERR_IO.
int64_t os64_work_pool_error(os64_work_pool_t *);

// Cancel, close the job writer, join workers, then release unreaped jobs
// and close remaining handles. Callback cooperation bounds the wait.
// NULL is harmless. Do this before destroying the window or shared state.
void os64_work_pool_destroy(os64_work_pool_t *);
#endif
