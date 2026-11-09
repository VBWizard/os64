// mathtest — libmath.so as a guest program meets it (MATH.md § Validation).
//
// The host test (tools/test_math_host.sh) judges the numbers: MPFR checks
// every vector, and the pinned source built on the host agrees bit for bit
// with the objects libmath.so is linked from. What only the guest can show
// is that the LIBRARY os64 loads computes those same bits — that the
// kernel's dynamic linker, its relocation of the prelinked image and its
// per-thread floating-point state hand the code the machine it was tested
// on. So this fixture:
//
//   1. recomputes every corpus block (corpus.h) through the library and
//      compares its digest with the host's (expected.h) — on a mismatch it
//      prints the block's vectors, to set beside
//      `tools/test_math_host.sh dump <fn> <block>` on the host;
//   2. names the libmath.so it ran, from /sys/shlib, with its pages now
//      resident;
//   3. runs the floating-point state checks (state.h);
//   4. does both again from four threads at once, two recomputing the
//      corpus and yielding between blocks, two holding a directed rounding
//      mode and checking that every answer and every control bit stays
//      theirs while the others run — the case a scheduler that leaked
//      MXCSR between threads would get wrong.
//
// Exit codes name the failed step: 0x3A740000 is a pass.

#include "os64/os64.h"
#include "os64/slurp.h"
#include "os64/thread.h"
#include <stdint.h>
#include <math.h>

#define MATH_REPORT(...) os64_printf(__VA_ARGS__)
#include "corpus.h"
#include "state.h"
#include "expected.h"

#define MATHTEST_OK            0x3A740000
#define MATHTEST_NOT_LOADED    0x3A740001   // no libmath.so in /sys/shlib
#define MATHTEST_CORPUS        0x3A740002   // a block digest differs from the host's
#define MATHTEST_STATE         0x3A740003   // a floating-point state check failed
#define MATHTEST_NO_THREAD     0x3A740004   // a worker could not be started or joined
#define MATHTEST_THREADED      0x3A740005   // a check failed only with threads running
#define MATHTEST_STALE         0x3A740006   // corpus.h and expected.h describe different corpora

#define CORPUS_THREADS   2
#define MODE_THREADS     2
#define CORPUS_PASSES    3
#define MODE_ITERATIONS  200000

static int expected_layout_matches(void)
{
    int vectors = 0, blocks = 0;
    for (int f = 0; f < MATH_FN_COUNT; f++) {
        vectors += math_vector_count(&kMathFns[f]);
        blocks += math_block_count(&kMathFns[f]);
    }
    return vectors == MATH_EXPECTED_VECTORS && blocks == MATH_COUNT(kMathExpected);
}

// The /sys/shlib stanza for libmath.so, printed whole: which file, where
// it landed, how many pages are resident and who else is using it.
static int show_loaded_library(void)
{
    uint8_t *text = 0;
    size_t len = 0;
    os64_slurp_status_t st = os64_slurp("/sys/shlib", 256 * 1024, &text, &len);
    if (st != OS64_SLURP_OK) {
        os64_printf("mathtest: cannot read /sys/shlib (%s)\n", os64_slurp_status_name(st));
        return 0;
    }
    static const char kWant[] = "object: /lib/libmath.so\n";
    int found = 0;
    size_t i = 0;
    while (i < len && !found) {
        size_t end = i;
        while (end < len && text[end] != '\n')
            end++;
        size_t n = 0;
        while (kWant[n] && i + n < len && text[i + n] == (uint8_t)kWant[n])
            n++;
        if (kWant[n] == 0) {
            found = 1;
            // The object line, then its indented detail lines.
            size_t stop = end + 1;
            while (stop < len && text[stop] == ' ') {
                while (stop < len && text[stop] != '\n')
                    stop++;
                stop++;
            }
            os64_write(1, text + i, (stop > len ? len : stop) - i);
        }
        i = end + 1;
    }
    os64_free(text);
    return found;
}

static void dump_block(int f, int block)
{
    int end = (block + 1) * MATH_BLOCK, n = math_vector_count(&kMathFns[f]);
    for (int i = block * MATH_BLOCK; i < end && i < n; i++) {
        uint64_t x, y;
        math_vector(f, i, &x, &y);
        os64_printf("%s %d %016llx %016llx %016llx\n", kMathFns[f].name, i,
                    (unsigned long long)x, (unsigned long long)y,
                    (unsigned long long)math_result(f, x, y));
    }
}

// Every block's digest against the host's. `verbose` prints and dumps a
// mismatch; a worker thread only counts, so four threads cannot interleave
// four dumps into one unreadable one.
static int check_corpus(int verbose, int yield)
{
    int mismatches = 0, k = 0;
    for (int f = 0; f < MATH_FN_COUNT; f++) {
        for (int b = 0; b < math_block_count(&kMathFns[f]); b++, k++) {
            uint64_t got = math_block_digest(f, b);
            if (got != kMathExpected[k]) {
                mismatches++;
                if (verbose) {
                    os64_printf("  FAIL %s block %d: digest %016llx, host %016llx\n",
                                kMathFns[f].name, b, (unsigned long long)got,
                                (unsigned long long)kMathExpected[k]);
                    dump_block(f, b);
                }
            }
            if (yield)
                os64_yield();
        }
    }
    return mismatches;
}

static int64_t corpus_worker(void *arg)
{
    (void)arg;
    math_set_mxcsr(MXCSR_DEFAULT);
    int failures = 0;
    for (int pass = 0; pass < CORPUS_PASSES; pass++)
        failures += check_corpus(0, 1);
    return failures;
}

// Holds one directed rounding mode for its whole life and checks that the
// library keeps answering in it: lrint against the known table, sqrt(2)
// against the value this thread computed before anyone else was running
// (sqrt is correctly rounded, so the mode decides its last bit), and the
// control half of MXCSR against what was loaded.
static int64_t mode_worker(void *arg)
{
    static const long expect[4][2] = {
        [MATH_RN] = { 2, -2 }, [MATH_RD] = { 1, -2 },
        [MATH_RU] = { 2, -1 }, [MATH_RZ] = { 1, -1 },
    };
    int mode = (int)(intptr_t)arg;
    uint32_t control = math_enter(mode) & ~MXCSR_FLAGS;
    volatile double a = 1.75, b = -1.75, two = 2.0;
    uint64_t root = math_to_bits(sqrt(two));
    int failures = 0;
    for (int i = 0; i < MODE_ITERATIONS && failures < 8; i++) {
        long ra = lrint(a), rb = lrint(b);
        uint64_t r = math_to_bits(sqrt(two));
        if (ra != expect[mode][0] || rb != expect[mode][1] || r != root ||
            (math_mxcsr() & ~MXCSR_FLAGS) != control)
            failures++;
        if ((i & 1023) == 0)
            os64_yield();
    }
    return failures;
}

static int run_threads(int *failures)
{
    int64_t handles[CORPUS_THREADS + MODE_THREADS];
    int n = 0;
    for (int i = 0; i < CORPUS_THREADS; i++)
        handles[n++] = os64_thread(corpus_worker, 0);
    handles[n++] = os64_thread(mode_worker, (void *)(intptr_t)MATH_RD);
    handles[n++] = os64_thread(mode_worker, (void *)(intptr_t)MATH_RU);
    int ok = 1;
    *failures = 0;
    for (int i = 0; i < n; i++) {
        int64_t result = 0;
        if (handles[i] < 0 || os64_thread_join((int32_t)handles[i], &result) < 0) {
            ok = 0;
            continue;
        }
        if (result != 0)
            os64_printf("  FAIL thread %d: %ld failed checks\n", i, (long)result);
        *failures += (int)result;
    }
    return ok;
}

int main(void)
{
    if (!expected_layout_matches()) {
        os64_printf("mathtest: corpus.h and expected.h disagree on the corpus size; "
                    "run tools/test_math_host.sh --regen\n");
        return MATHTEST_STALE;
    }
    // The corpus runs first, so the stanza printed after it shows the
    // library's pages resident: the calls ran in THIS file's code.
    int bad = check_corpus(1, 0);
    if (!show_loaded_library()) {
        os64_printf("mathtest: FAIL no /lib/libmath.so in /sys/shlib\n");
        return MATHTEST_NOT_LOADED;
    }
    os64_printf("mathtest: corpus %s (%d vectors, %d of %d blocks differ from the host)\n",
                bad ? "FAIL" : "PASS", MATH_EXPECTED_VECTORS, bad, MATH_COUNT(kMathExpected));
    if (bad)
        return MATHTEST_CORPUS;

    int state = math_state_checks();
    os64_printf("mathtest: state %s (%d failures)\n", state ? "FAIL" : "PASS", state);
    if (state)
        return MATHTEST_STATE;

    int threaded = 0;
    if (!run_threads(&threaded)) {
        os64_printf("mathtest: FAIL could not start or join a worker\n");
        return MATHTEST_NO_THREAD;
    }
    os64_printf("mathtest: threads %s (%d corpus x%d, %d rounding-mode x%d, %d failures)\n",
                threaded ? "FAIL" : "PASS", CORPUS_THREADS, CORPUS_PASSES,
                MODE_THREADS, MODE_ITERATIONS, threaded);
    if (threaded)
        return MATHTEST_THREADED;

    os64_printf("mathtest: PASS\n");
    return MATHTEST_OK;
}
