// js — run a JavaScript program. USAGE.md beside this file is both the
// user's guide and the specification this file implements: the grammar, what
// scripts see in scriptArgs, and the exit statuses.
//
// The runner is THIN on purpose (JAVASCRIPT_TASKS.md § C1): it turns a
// command line into one call sequence on <os64/js.h> and turns the outcome
// into a message and an exit status. Loading, evaluation, job draining and
// every budget are the library's. Nothing here touches QuickJS, and nothing
// loops over jobs: the library's run operations drain the queue themselves.

#include "os64/os64.h"
#include "os64/js.h"

// The exit statuses USAGE.md § Exit status promises. The library's own fatal
// exit (OS64_JS_FATAL_EXIT) never passes through here.
#define JS_EXIT_OK      0
#define JS_EXIT_SCRIPT  1   // an uncaught exception or unhandled rejection
#define JS_EXIT_USAGE   2
#define JS_EXIT_IO      3   // the script could not be read or held, or output written
#define JS_EXIT_BUDGET  4
#define JS_EXIT_REFUSED 5   // the library refused, or answered outside the contract

// QuickJS's stack check runs on the program's own 1 MiB thread stack
// (THREAD_USER_STACK_SIZE). The cap leaves nominal host headroom; J2 measures
// recursion and a bounded native callback at this cap. Arbitrary host/native
// frames still need their own budget. Engine stack overflow is an exception.
#define JS_STACK_CAP ((size_t)768 * 1024)

typedef enum { JS_FROM_FILE, JS_FROM_EVAL, JS_FROM_STDIN } js_source_kind_t;

typedef struct {
    js_source_kind_t kind;
    const char *operand;    // the path, the -e SOURCE, or "-"
    const char *name;       // scriptArgs[0] and the diagnostics' source name
    int32_t extra_count;    // the script's own arguments, after the operand
    char **extra;
    os64_js_limits_t limits;
} js_plan_t;

static const os64_optspec_t kSpecs[] = {
    { .letter = 'e', .name = "eval", .takes_value = true,
      .help = "run SOURCE instead of a file" },
    { .letter = 'm', .name = "memory", .takes_value = true,
      .help = "memory budget, bytes or K/M (default 64M)" },
    { .letter = 't', .name = "time", .takes_value = true,
      .help = "execution budget in milliseconds (default 60000)" },
    { .name = "stack", .takes_value = true,
      .help = "JavaScript stack budget, at most 768K (default 256K)" },
    { .name = "source", .takes_value = true,
      .help = "largest script accepted (default 4M)" },
    { .name = "jobs", .takes_value = true,
      .help = "most Promise jobs one run may execute (default 2^64-1)" },
};
#define JS_SPEC_COUNT ((int32_t)(sizeof(kSpecs) / sizeof(kSpecs[0])))
#define JS_USAGE "js [options] FILE|-e SOURCE|- [ARG...]"

// "64M", "256K", "4096": a positive byte count with an optional binary suffix.
static bool parse_size(const char *text, uint64_t *out)
{
    char digits[24];
    size_t n = os64_strlen(text);
    uint64_t scale = 1;
    if (n > 0 && (text[n - 1] == 'K' || text[n - 1] == 'k')) { scale = 1024; n--; }
    else if (n > 0 && (text[n - 1] == 'M' || text[n - 1] == 'm')) { scale = 1024 * 1024; n--; }
    if (n == 0 || n >= sizeof(digits))
        return false;
    os64_memcpy(digits, text, n);
    digits[n] = '\0';
    uint64_t value;
    if (!os64_parse_u64(digits, &value) || value == 0 || value > UINT64_MAX / scale)
        return false;
    *out = value * scale;
    return true;
}

static bool parse_count(const char *text, uint64_t *out)
{
    return os64_parse_u64(text, out) && *out != 0;
}

// The name of the long option a token spells: "--stack=1M" and "--stack" are
// both "stack". Needed because a long-only option comes back from
// os64_args_next as letter zero, whichever of them it was.
static bool token_names(const char *token, const char *name)
{
    if (token[0] != '-' || token[1] != '-')
        return false;
    token += 2;
    while (*name != '\0' && *token == *name)
        token++, name++;
    return *name == '\0' && (*token == '\0' || *token == '=');
}

static int32_t usage_error(os64_args_t *a, const char *why, const char *what)
{
    os64_hprintf(2, "js: %s: %s\n", why, what);
    os64_args_help(a, JS_USAGE);
    return JS_EXIT_USAGE;
}

// argv -> plan. Options are read until the first operand (a file, "-", or
// -e's SOURCE); everything after it is the script's, flags included, so the
// parser is driven by hand rather than run to completion.
static int32_t parse_plan(int32_t argc, char **argv, js_plan_t *plan)
{
    os64_args_t a;
    os64_args_init(&a, argc, argv, kSpecs, JS_SPEC_COUNT);
    a.about = "run a JavaScript program: FILE, -e SOURCE, or - for standard input";
    a.details = "after the script, every argument is the script's own (scriptArgs)";

    plan->limits = os64_js_default_limits();

    for (;;) {
        // A long-only option is identified by the token this call starts
        // on: a pending "-la" bundle only ever yields letters, never zero.
        const char *token = a.index < a.argc ? a.argv[a.index] : "";
        int32_t r = os64_args_next(&a);
        uint64_t n;
        switch (r) {
        case OS64_ARG_END:
            return usage_error(&a, "no script", "name a FILE, -e SOURCE, or -");
        case OS64_ARG_HELP:
            os64_args_help(&a, JS_USAGE);
            return -1;
        case OS64_ARG_ERROR:
            return usage_error(&a, "unknown option or missing value", a.value);
        case 'e':
            plan->kind = JS_FROM_EVAL;
            plan->operand = a.value;
            plan->name = "-e";
            goto operand_seen;
        case OS64_ARG_POSITIONAL:
            plan->kind = os64_streq(a.value, "-") ? JS_FROM_STDIN : JS_FROM_FILE;
            plan->operand = a.value;
            plan->name = a.value;
            goto operand_seen;
        case 'm':
            if (!parse_size(a.value, &n) || n > SIZE_MAX)
                return usage_error(&a, "--memory wants a positive size like 64M", a.value);
            plan->limits.memory_bytes = (size_t)n;
            break;
        case 't':
            if (!parse_count(a.value, &n))
                return usage_error(&a, "--time wants a positive count of milliseconds", a.value);
            plan->limits.execution_ms = n;
            break;
        case 0:
            if (token_names(token, "stack")) {
                if (!parse_size(a.value, &n) || n > JS_STACK_CAP)
                    return usage_error(&a, "--stack wants a positive size, at most 768K", a.value);
                plan->limits.stack_bytes = (size_t)n;
            } else if (token_names(token, "source")) {
                if (!parse_size(a.value, &n) || n >= SIZE_MAX)
                    return usage_error(&a, "--source wants a positive size like 4M", a.value);
                plan->limits.source_bytes = (size_t)n;
            } else if (token_names(token, "jobs")) {
                if (!parse_count(a.value, &n))
                    return usage_error(&a, "--jobs wants a positive count", a.value);
                plan->limits.jobs_per_turn = n;
            } else {
                return usage_error(&a, "unknown option", token);
            }
            break;
        default:
            return usage_error(&a, "unknown option", token);
        }
    }
operand_seen:
    plan->extra_count = a.argc - a.index;
    plan->extra = a.argv + a.index;
    return JS_EXIT_OK;
}

// Standard input, whole, into one buffer. At most source_bytes + 1 bytes are
// read: one byte past the budget is enough for the library to refuse the
// script as too large (the library owns that verdict, as it does for files),
// and a stream that never ends cannot make the runner allocate without bound.
static int32_t read_stdin(size_t limit, char **out, size_t *out_length)
{
    size_t cap = 4096, length = 0, want = limit + 1;
    char *buffer = os64_malloc(cap);
    if (buffer == NULL) {
        os64_hprintf(2, "js: -: no memory to read standard input\n");
        return JS_EXIT_IO;
    }
    while (length < want) {
        if (length == cap) {
            size_t grown = cap * 2 < want ? cap * 2 : want;
            char *next = os64_realloc(buffer, grown);
            if (next == NULL) {
                os64_free(buffer);
                os64_hprintf(2, "js: -: no memory to read standard input\n");
                return JS_EXIT_IO;
            }
            buffer = next;
            cap = grown;
        }
        size_t room = (cap < want ? cap : want) - length;
        int64_t got = os64_read(0, buffer + length, room);
        if (got == 0)
            break;
        if (got < 0) {
            os64_free(buffer);
            os64_hprintf(2, "js: -: cannot read standard input (error %ld)\n", (long)got);
            return JS_EXIT_IO;
        }
        length += (size_t)got;
    }
    *out = buffer;
    *out_length = length;
    return JS_EXIT_OK;
}

// "64M", "256K", or plain bytes: a budget as the person would have typed it.
static void print_size(uint64_t bytes)
{
    if (bytes % (1024 * 1024) == 0)
        os64_hprintf(2, "%luM", (unsigned long)(bytes / (1024 * 1024)));
    else if (bytes % 1024 == 0)
        os64_hprintf(2, "%luK", (unsigned long)(bytes / 1024));
    else
        os64_hprintf(2, "%lu bytes", (unsigned long)bytes);
}

static const char *status_name(os64_js_status_t status)
{
    switch (status) {
    case OS64_JS_OK: return "OK";
    case OS64_JS_MORE_JOBS: return "MORE_JOBS";
    case OS64_JS_EXCEPTION: return "EXCEPTION";
    case OS64_JS_UNHANDLED_REJECTION: return "UNHANDLED_REJECTION";
    case OS64_JS_LIMIT: return "LIMIT";
    case OS64_JS_CANCELLED: return "CANCELLED";
    case OS64_JS_HOST_FAILURE: return "HOST_FAILURE";
    case OS64_JS_BAD_ARGUMENT: return "BAD_ARGUMENT";
    case OS64_JS_ABI_MISMATCH: return "ABI_MISMATCH";
    case OS64_JS_BUSY: return "BUSY";
    case OS64_JS_FAILED_RUNTIME: return "FAILED_RUNTIME";
    }
    return "an unknown status";
}

// The outcome as one line on handle 2, then the stack trace when there is
// one, and the exit status it maps to. The library composes the message; the
// runner frames it with what ran and where.
static int32_t report(const js_plan_t *plan, const os64_js_outcome_t *o)
{
    const char *source = o->source_name[0] != '\0' ? o->source_name : plan->name;
    // diagnostic_truncated covers every field, and the stack trace is the one
    // that overflows (a runaway recursion's trace is the same frame a
    // thousand times), so with a trace the marker ends the trace rather than
    // making a whole one-line message look cut.
    bool traced = o->stack_trace[0] != '\0';
    const char *cut = o->diagnostic_truncated && !traced ? " [truncated]" : "";
    int32_t status;

    switch (o->status) {
    case OS64_JS_OK:
        return JS_EXIT_OK;
    case OS64_JS_EXCEPTION:
        if (o->line != 0)
            os64_hprintf(2, "js: %s:%u:%u: %s%s\n", source, o->line, o->column, o->message, cut);
        else
            os64_hprintf(2, "js: %s: %s%s\n", source, o->message, cut);
        status = JS_EXIT_SCRIPT;
        break;
    case OS64_JS_UNHANDLED_REJECTION:
        os64_hprintf(2, "js: %s: unhandled promise rejection: %s%s\n", source, o->message, cut);
        status = JS_EXIT_SCRIPT;
        break;
    case OS64_JS_LIMIT:
        os64_hprintf(2, "js: %s: ", source);
        switch (o->limit) {
        case OS64_JS_LIMIT_MEMORY:
            os64_hprintf(2, "memory budget exceeded (");
            print_size(plan->limits.memory_bytes);
            break;
        case OS64_JS_LIMIT_STACK:
            os64_hprintf(2, "stack budget exceeded (");
            print_size(plan->limits.stack_bytes);
            break;
        case OS64_JS_LIMIT_SOURCE:
            os64_hprintf(2, "source budget exceeded (");
            print_size(plan->limits.source_bytes);
            break;
        case OS64_JS_LIMIT_EXECUTION:
            os64_hprintf(2, "time budget exceeded (%lu ms",
                         (unsigned long)plan->limits.execution_ms);
            break;
        case OS64_JS_LIMIT_JOBS:
            os64_hprintf(2, "jobs budget exceeded (%lu jobs",
                         (unsigned long)plan->limits.jobs_per_turn);
            break;
        default:
            os64_hprintf(2, "a budget was exceeded (unnamed");
            break;
        }
        os64_hprintf(2, ")\n");
        return JS_EXIT_BUDGET;
    case OS64_JS_HOST_FAILURE:
        // host_error zero means the library had no service code to give
        // (an allocation refusal, a write that made no progress).
        os64_hprintf(2, "js: %s: %s", source,
                     o->message[0] != '\0' ? o->message : "input or output failed");
        if (o->host_error != 0)
            os64_hprintf(2, " (error %ld)", (long)o->host_error);
        os64_hprintf(2, "%s\n", cut);
        return JS_EXIT_IO;
    default:
        // Statuses the runner's call sequence never invites (MORE_JOBS,
        // BUSY, CANCELLED, FAILED_RUNTIME) and the library's refusals
        // (BAD_ARGUMENT, ABI_MISMATCH): name the status, since the person
        // reading this is debugging an install or the library, not a script.
        os64_hprintf(2, "js: %s: the JavaScript library answered %s%s%s\n", source,
                     status_name(o->status), o->message[0] != '\0' ? ": " : "", o->message);
        return JS_EXIT_REFUSED;
    }
    if (traced) {
        size_t n = os64_strlen(o->stack_trace);
        os64_write(2, o->stack_trace, n);
        if (o->stack_trace[n - 1] != '\n')
            os64_write(2, "\n", 1);
        if (o->diagnostic_truncated)
            os64_hprintf(2, "    [truncated]\n");
    }
    return status;
}

int main(int argc, char **argv)
{
    js_plan_t plan = {0};
    int32_t parsed = parse_plan(argc, argv, &plan);
    if (parsed < 0)
        return JS_EXIT_OK;          // --help
    if (parsed != JS_EXIT_OK)
        return parsed;

    // A file the library cannot load comes back as HOST_FAILURE, the same
    // status a failed write to standard output gets, so the commonest
    // mistake (a mistyped path) is named here, before any runtime exists.
    // A file that vanishes after this check is still reported by the library.
    if (plan.kind == JS_FROM_FILE) {
        os64_dirent_t entry;
        if (os64_stat(plan.operand, &entry) < 0) {
            os64_hprintf(2, "js: %s: no such file\n", plan.operand);
            return JS_EXIT_IO;
        }
        if (entry.flags & OS64_DE_DIR) {
            os64_hprintf(2, "js: %s: is a directory\n", plan.operand);
            return JS_EXIT_IO;
        }
    }

    char *input = NULL;
    size_t input_length = 0;
    if (plan.kind == JS_FROM_STDIN) {
        int32_t r = read_stdin(plan.limits.source_bytes, &input, &input_length);
        if (r != JS_EXIT_OK)
            return r;
    }

    // scriptArgs: what ran, then the script's own arguments.
    const char **args = os64_malloc(sizeof(char *) * (size_t)(plan.extra_count + 1));
    if (args == NULL) {
        os64_free(input);
        os64_hprintf(2, "js: no memory for the script's arguments\n");
        return JS_EXIT_IO;
    }
    args[0] = plan.name;
    for (int32_t i = 0; i < plan.extra_count; i++)
        args[i + 1] = plan.extra[i];

    os64_js_config_t config = { plan.limits };
    os64_js_outcome_t outcome;
    os64_js_runtime_t *runtime = NULL;
    os64_js_status_t status = os64_js_create(&config, OS64_JS_ABI_ID, &runtime, &outcome);
    if (status == OS64_JS_OK)
        status = os64_js_install_output(runtime, 1,
                                        OS64_JS_OUTPUT_PRINT | OS64_JS_OUTPUT_CONSOLE_LOG,
                                        &outcome);
    if (status == OS64_JS_OK)
        status = os64_js_install_args(runtime, (size_t)plan.extra_count + 1, args, &outcome);
    if (status == OS64_JS_OK) {
        if (plan.kind == JS_FROM_FILE)
            status = os64_js_run_file(runtime, plan.operand, &outcome);
        else if (plan.kind == JS_FROM_EVAL)
            status = os64_js_run(runtime, plan.operand, os64_strlen(plan.operand),
                                 plan.name, &outcome);
        else
            status = os64_js_run(runtime, input, input_length, plan.name, &outcome);
    }
    (void)status;   // the outcome carries the same status (CONTRACT.md § Results)

    int32_t exit_status = report(&plan, &outcome);
    os64_js_destroy(runtime);       // NULL when creation failed: a no-op
    os64_free(args);
    os64_free(input);
    return exit_status;
}
