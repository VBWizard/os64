#ifndef OS64_JS_H
#define OS64_JS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Embedding ABI. Operation contracts, supported symbols and examples are
 * documented in userland/libjs/CONTRACT.md. */
#define OS64_JS_ABI_ID "os64-js/2;quickjs=2026-06-04;lp64;value=16;limb=64;atomics=0"
#define OS64_JS_MESSAGE_CAP 256
#define OS64_JS_SOURCE_NAME_CAP 128
#define OS64_JS_STACK_TRACE_CAP 1024
#define OS64_JS_FATAL_EXIT ((int32_t)0x4A534641) /* JSFA */

typedef struct os64_js_runtime os64_js_runtime_t;

typedef enum {
    OS64_JS_OK = 0,
    OS64_JS_MORE_JOBS,
    OS64_JS_EXCEPTION,
    OS64_JS_UNHANDLED_REJECTION,
    OS64_JS_LIMIT,
    OS64_JS_CANCELLED,
    OS64_JS_HOST_FAILURE,
    OS64_JS_BAD_ARGUMENT,
    OS64_JS_ABI_MISMATCH,
    OS64_JS_BUSY,
    OS64_JS_FAILED_RUNTIME
} os64_js_status_t;

typedef enum {
    OS64_JS_LIMIT_NONE = 0,
    OS64_JS_LIMIT_SOURCE,
    OS64_JS_LIMIT_MEMORY,
    OS64_JS_LIMIT_STACK,
    OS64_JS_LIMIT_EXECUTION,
    OS64_JS_LIMIT_JOBS
} os64_js_limit_t;

/* Select the output names the host grants; zero or unknown bits are invalid. */
typedef enum {
    OS64_JS_OUTPUT_PRINT = 1u << 0,
    OS64_JS_OUTPUT_CONSOLE = 1u << 1
} os64_js_output_names_t;

/* Inline diagnostic storage: no allocation or borrowed strings to release.
 * Truncated/unavailable fields do not change the structured status. Locations
 * are one-based; zero denotes unavailable. host_error preserves an available
 * os64 error code; zero means that no service code is available. */
typedef struct {
    os64_js_status_t status;
    os64_js_limit_t limit;
    int64_t host_error;
    uint32_t line, column;
    uint64_t jobs_executed;
    bool jobs_pending;
    bool diagnostic_truncated;
    char message[OS64_JS_MESSAGE_CAP];
    char source_name[OS64_JS_SOURCE_NAME_CAP];
    char stack_trace[OS64_JS_STACK_TRACE_CAP];
} os64_js_outcome_t;

typedef struct {
    size_t memory_bytes;
    size_t stack_bytes;
    size_t source_bytes;
    uint64_t execution_ms;
    uint64_t jobs_per_turn;
} os64_js_limits_t;

/* Standalone defaults for os64's 1 MiB native thread stack. Hosts with native
 * bindings budget their own stack/heap work; browser limits need separate
 * measurements. The finite deadline bounds ordinary Promise work; callers
 * may lower the job count for a deterministic cap. */
static inline os64_js_limits_t os64_js_default_limits(void)
{
    const os64_js_limits_t limits = {
        (size_t)64 * 1024 * 1024, (size_t)256 * 1024,
        (size_t)4 * 1024 * 1024, UINT64_C(60000), UINT64_MAX
    };
    return limits;
}

/* Callers supply positive, representable limits; creation requires an explicit
 * configuration. os64_js_default_limits() supplies the standalone profile. */
typedef struct {
    os64_js_limits_t limits;
} os64_js_config_t;

/* Reclamation is for audited hosts whose C-held values are drained before
 * destroy and whose finalizers own no native resources. Other engine aborts
 * remain fatal. The ordinary create entry selects FATAL. */
typedef enum {
    OS64_JS_TEARDOWN_FATAL = 0,
    OS64_JS_TEARDOWN_RECLAIM = 1
} os64_js_teardown_policy_t;

typedef struct {
    bool leaked;
    size_t reclaimed_blocks;
    size_t reclaimed_bytes; /* allocator charge, including ledger headers */
} os64_js_teardown_report_t;

/* Outcome and output pointers are required. Source bytes and names are borrowed
 * during the call; the library supplies engine-required termination/storage.
 * Caller-header identity is checked before runtime/context publication. */
os64_js_status_t os64_js_create(const os64_js_config_t *config,
                                const char *caller_abi,
                                os64_js_runtime_t **out,
                                os64_js_outcome_t *outcome);
os64_js_status_t os64_js_create_with_teardown(const os64_js_config_t *config,
                                              os64_js_teardown_policy_t policy,
                                              const char *caller_abi,
                                              os64_js_runtime_t **out,
                                              os64_js_outcome_t *outcome);
os64_js_status_t os64_js_eval(os64_js_runtime_t *runtime,
                              const void *source, size_t length,
                              const char *source_name,
                              os64_js_outcome_t *outcome);
os64_js_status_t os64_js_run(os64_js_runtime_t *runtime,
                             const void *source, size_t length,
                             const char *source_name,
                             os64_js_outcome_t *outcome);
os64_js_status_t os64_js_run_file(os64_js_runtime_t *runtime,
                                  const char *path,
                                  os64_js_outcome_t *outcome);
/* A slice yields MORE_JOBS without resetting the turn's deadline or job cap.
 * An empty runnable queue is OK; unresolved promises are not external work. */
os64_js_status_t os64_js_drain_jobs(os64_js_runtime_t *runtime,
                                    uint64_t slice_jobs,
                                    os64_js_outcome_t *outcome);
/* Output borrows the handle and installs only the selected names. Arguments
 * are copied. These installers expose no script-visible file APIs. */
os64_js_status_t os64_js_install_output(os64_js_runtime_t *runtime,
                                        int32_t handle, uint32_t names,
                                        os64_js_outcome_t *outcome);
os64_js_status_t os64_js_install_args(os64_js_runtime_t *runtime,
                                      size_t count, const char *const *args,
                                      os64_js_outcome_t *outcome);
/* Cancellation is sticky. This is the cross-thread operation; callers keep the
 * runtime alive until requesting threads have stopped using its handle. */
void os64_js_cancel(os64_js_runtime_t *runtime);
/* Owner-thread only, outside active calls/callbacks. Both destroy entries
 * consume the runtime and invalidate its context/values. NULL is a no-op.
 * Reporting destruction follows the creation policy; FATAL retains its
 * object/weakref assertions and returns a zero report after clean destroy.
 * RECLAIM reports allocator leftovers without further engine entry. Hosts must
 * inspect/log a nonzero report. Other invariant violations remain fatal.
 * A NULL report discards the verdict, as the void destroy entry does. */
void os64_js_destroy_report(os64_js_runtime_t *runtime,
                             os64_js_teardown_report_t *report);
void os64_js_destroy(os64_js_runtime_t *runtime);
#endif
