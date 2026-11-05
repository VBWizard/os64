/* test262's runner (INTL.md § 8): each prepared test (test262's harness
 * and the test, as tools/test262/run.py writes them) in a fresh runtime of
 * the real libjs, the target engine with Intl installed as the js command
 * installs it, on controlled OS services. It judges each one as test262's
 * INTERPRETING.md says and prints one line per test; run.py holds the
 * lines to the expectation files.
 *
 *   runner <list>     each line: id TAB path TAB async(0|1) TAB negative type or -
 *   output            id TAB PASS, or id TAB FAIL TAB why
 */
#define _POSIX_C_SOURCE 200809L
#include <stdalign.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "os64/date.h"
#include "os64/js.h"

/* ── The OS services libjs and libos64's calendar ask for ──────────────── */
typedef union { max_align_t alignment; struct { size_t size; } metadata; } Block;
os64_time_t js_test_clock = {.epoch = 1700000000, .ticks_per_second = 100};
int js_test_clock_failed;

void *os64_malloc(size_t size)
{
    if (size > SIZE_MAX - sizeof(Block) - 15) return NULL;
    size = (size + 15) & ~(size_t)15;
    Block *block = malloc(sizeof(*block) + size);
    if (!block) return NULL;
    block->metadata.size = size;
    return block + 1;
}
size_t os64_malloc_size(const void *pointer) { return ((const Block *)pointer)[-1].metadata.size; }
void os64_free(void *pointer) { if (pointer) free((Block *)pointer - 1); }
void *os64_realloc(void *pointer, size_t size)
{
    if (!pointer) return os64_malloc(size);
    if (!size) { os64_free(pointer); return NULL; }
    void *replacement = os64_malloc(size);
    if (replacement) {
        size_t old = os64_malloc_size(pointer);
        memcpy(replacement, pointer, size < old ? size : old);
        os64_free(pointer);
    }
    return replacement;
}
const char *os64_getenv(const char *name) { return getenv(name); }
int64_t os64_micros(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return -1;
    return now.tv_sec * INT64_C(1000000) + now.tv_nsec / 1000;
}

/* What a test prints (its async verdict) is kept, not shown. */
static char printed[4096];
static size_t printed_length;
int64_t os64_write(int32_t handle, const void *bytes, size_t size)
{
    if (handle != 1) return (int64_t)fwrite(bytes, 1, size, stderr);
    size_t room = sizeof(printed) - 1 - printed_length;
    size_t n = size < room ? size : room;
    memcpy(printed + printed_length, bytes, n);
    printed_length += n;
    printed[printed_length] = '\0';
    return (int64_t)size;
}
int64_t os64_open(const char *path, const char *mode) { (void)path; (void)mode; return -84; }
int64_t os64_read(int32_t handle, void *bytes, size_t size) { (void)handle; (void)bytes; (void)size; return -84; }
int64_t os64_close(int32_t handle) { (void)handle; return -84; }
void os64_exit(int32_t status)
{
    fprintf(stderr, "test262 runner: target exit %08x\n", (unsigned)status);
    _Exit(98);
}

/* ── One test ──────────────────────────────────────────────────────────── */
static char *slurp(const char *path, size_t *length)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = size >= 0 ? malloc((size_t)size + 1) : NULL;
    if (text != NULL && fread(text, 1, (size_t)size, f) != (size_t)size) { free(text); text = NULL; }
    fclose(f);
    if (text != NULL) { text[size] = '\0'; *length = (size_t)size; }
    return text;
}

/* The why of a failure, on one line. */
static void flatten(char *text)
{
    for (; *text; text++) if (*text == '\t' || *text == '\n' || *text == '\r') *text = ' ';
}

static void run_one(const char *id, const char *path, int async, const char *negative)
{
    size_t length = 0;
    char *source = slurp(path, &length);
    if (source == NULL) { printf("%s\tFAIL\tcannot read %s\n", id, path); return; }
    os64_js_config_t config = {{64u << 20, 1u << 20, 16u << 20, 10000, 1u << 20}};
    os64_js_runtime_t *runtime = NULL;
    os64_js_outcome_t outcome;
    printed_length = 0;
    printed[0] = '\0';
    os64_js_status_t status = os64_js_create(&config, OS64_JS_ABI_ID, &runtime, &outcome);
    if (status == OS64_JS_OK) status = os64_js_install_output(runtime, 1, OS64_JS_OUTPUT_PRINT, &outcome);
    if (status == OS64_JS_OK) status = os64_js_install_intl(runtime, OS64_JS_LOCALE, &outcome);
    if (status != OS64_JS_OK) {
        flatten(outcome.message);
        printf("%s\tFAIL\tsetup: %s\n", id, outcome.message);
    } else {
        status = os64_js_run(runtime, source, length, path, &outcome);
        flatten(outcome.message);
        if (negative[0] != '-') {
            size_t n = strlen(negative);
            if (status == OS64_JS_EXCEPTION && strncmp(outcome.message, negative, n) == 0 &&
                (outcome.message[n] == ':' || outcome.message[n] == '\0'))
                printf("%s\tPASS\n", id);
            else if (status == OS64_JS_OK)
                printf("%s\tFAIL\texpected %s, completed\n", id, negative);
            else
                printf("%s\tFAIL\texpected %s, got %s\n", id, negative, outcome.message);
        } else if (status != OS64_JS_OK) {
            printf("%s\tFAIL\t%s\n", id, outcome.message);
        } else if (async && strstr(printed, "Test262:AsyncTestComplete") == NULL) {
            flatten(printed);
            printf("%s\tFAIL\tasync: %s\n", id, printed[0] ? printed : "never completed");
        } else {
            printf("%s\tPASS\n", id);
        }
    }
    os64_js_destroy(runtime);
    free(source);
}

int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: runner <list>\n"); return 2; }
    FILE *list = fopen(argv[1], "r");
    if (list == NULL) { fprintf(stderr, "runner: cannot read %s\n", argv[1]); return 2; }
    char line[8192];
    while (fgets(line, sizeof(line), list) != NULL) {
        line[strcspn(line, "\n")] = '\0';
        char *id = strtok(line, "\t"), *path = strtok(NULL, "\t"), *async = strtok(NULL, "\t"),
             *negative = strtok(NULL, "\t");
        if (id == NULL || path == NULL || async == NULL || negative == NULL) {
            fprintf(stderr, "runner: a malformed list line\n");
            return 2;
        }
        run_one(id, path, async[0] == '1', negative);
        fflush(stdout);
    }
    fclose(list);
    return 0;
}
