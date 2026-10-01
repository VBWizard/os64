#ifndef OS64_JS_H
#define OS64_JS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Proposed R0 API, not implemented by a shipped library. The runtime contract
 * and examples live in userland/libjs/CONTRACT.md. */
#define OS64_JS_ABI_ID "os64-js/1;quickjs=2026-06-04;lp64;value=16;limb=64;atomics=0"
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

/* Inline diagnostic storage: no allocation or borrowed strings to release.
 * Truncated/unavailable fields do not change the structured status. Locations
 * are one-based; zero denotes unavailable. host_error is an os64 error code. */
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

/* Limits must be positive and finite. Production defaults are selected after
 * guest measurements; callers initialize this proposed API explicitly. */
typedef struct {
    os64_js_limits_t limits;
} os64_js_config_t;

/* Outcome and output pointers are required. Source bytes and names are borrowed
 * during the call; the library supplies engine-required termination/storage.
 * Caller-header identity is checked before runtime/context publication. */
os64_js_status_t os64_js_create(const os64_js_config_t *config,
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
/* These installers borrow the handle, copy arguments, and expose no file APIs. */
os64_js_status_t os64_js_install_output(os64_js_runtime_t *runtime,
                                        int32_t handle,
                                        os64_js_outcome_t *outcome);
os64_js_status_t os64_js_install_args(os64_js_runtime_t *runtime,
                                      size_t count, const char *const *args,
                                      os64_js_outcome_t *outcome);
/* Cancellation is sticky. This is the cross-thread operation; callers keep the
 * runtime alive until requesting threads have stopped using its handle. */
void os64_js_cancel(os64_js_runtime_t *runtime);
/* Owner-thread only, outside active calls/callbacks. NULL is a no-op. Engine
 * invariant violations use the fatal path, not a recoverable outcome. */
void os64_js_destroy(os64_js_runtime_t *runtime);
#endif
