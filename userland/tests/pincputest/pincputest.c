// pincputest — `PINCPU=<n>` pins a program, and every thread it makes, to
// CPU n (task.c task_env_pin).
//
// The parent pins a copy of itself to each running CPU in turn, by setting
// PINCPU in its own environment and spawning: the child inherits the
// variable exactly as `env PINCPU=3 prog` hands it over. The child then asks
// two witnesses where it is:
//
//   CPUID     the APIC ID of the CPU executing the instruction, read over
//             and over across yields and sleeps, from the main thread AND
//             from a second thread (which inherits the pin at birth, never
//             from the environment). A thread that is merely STARTED on the
//             right CPU and then wanders would fail here. CPUID speaks APIC
//             IDs, so the child translates through each /sys/cpu/<n>/state's
//             `apic:` line — on a machine with sparse IDs (a 3900X, or
//             `make run-sparse`) CPU n is not APIC n, and this is the check
//             that knows the difference.
//   /proc     each thread's `affinity` line, which is the pin itself: the
//             record has to say what the scheduler is obeying.
//
// Then PINCORE, the variable's old name: still honoured when PINCPU is
// absent, read as a CPU number, and outranked by PINCPU when both are set.
//
// Then the refusals, which must leave a program UNPINNED (`affinity any`)
// rather than pinned somewhere surprising: a word that is not a number, a
// CPU that is not running, and a number too long to be a CPU.
//
// Exit codes: 0xC02E0000 success, a distinct code per failure.

#include "os64/os64.h"

#define PCT_OK             0xC02E0000
#define PCT_NO_COUNT       0xC02E0001   // /sys/cpu/count unreadable
#define PCT_NO_SPAWN       0xC02E0002
#define PCT_NO_THREAD      0xC02E0003
#define PCT_WANDERED       0xC02E0004   // CPUID saw the pinned task on another CPU
#define PCT_BAD_RECORD     0xC02E0005   // a thread's /proc affinity is not the pin
#define PCT_NO_PROC        0xC02E0006   // /proc/self/thread unreadable
#define PCT_REFUSAL_PINNED 0xC02E0007   // a refused value pinned the program anyway
#define PCT_NO_SETENV      0xC02E0008
#define PCT_NO_APIC        0xC02E0009   // /sys/cpu/<n>/state has no apic line

#define SAMPLES 200

static uint32_t apic_now(void)
{
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    return b >> 24;
}

// Read a small synthetic file into buf, NUL-terminated. False on failure.
static bool read_small(const char *path, char *buf, size_t cap)
{
    int64_t h = os64_open(path, NULL);
    if (h < 0)
        return false;
    size_t used = 0;
    int64_t n;
    while (used + 1 < cap && (n = os64_read((int32_t)h, buf + used, cap - 1 - used)) > 0)
        used += (size_t)n;
    os64_close((int32_t)h);
    buf[used] = '\0';
    return used > 0;
}

// CPU `cpu`'s APIC ID, from its state file. False when the file or the line
// is missing.
static bool apic_of_cpu(uint64_t cpu, uint32_t *apic)
{
    char path[48], text[1024];
    os64_snprintf(path, sizeof(path), "/sys/cpu/%lu/state", cpu);
    if (!read_small(path, text, sizeof(text)))
        return false;
    for (const char *p = text; *p; p++)
    {
        if ((p == text || p[-1] == '\n') && os64_memcmp(p, "apic: ", 6) == 0)
        {
            uint64_t v = 0;
            const char *q = p + 6;
            if (*q < '0' || *q > '9')
                return false;
            while (*q >= '0' && *q <= '9')
                v = v * 10 + (uint64_t)(*q++ - '0');
            *apic = (uint32_t)v;
            return true;
        }
    }
    return false;
}

// Every sample, from whichever thread runs this, must be on the CPU whose
// APIC ID is `arg`. The yields and sleeps are what give the scheduler its
// chances to move it.
static int64_t sample(void *arg)
{
    uint32_t want = (uint32_t)(uintptr_t)arg;
    for (int i = 0; i < SAMPLES; i++)
    {
        if (apic_now() != want)
            return PCT_WANDERED;
        if (i % 50 == 49)
            os64_sleep(10);
        else
            os64_yield();
    }
    return 0;
}

// Every thread of this task must carry `want` in its /proc affinity line
// ("any" when want is NULL). Called while the second thread is still alive,
// so both are listed.
static uint32_t check_records(const char *want)
{
    int64_t d = os64_opendir("/proc/self/thread");
    if (d < 0)
        return PCT_NO_PROC;
    int seen = 0;
    uint32_t verdict = 0;
    os64_dirent_t e;
    while (verdict == 0 && os64_readdir((int32_t)d, &e) == 1)
    {
        char path[96], text[1024], line[32];
        os64_snprintf(path, sizeof(path), "/proc/self/thread/%s/status", e.name);
        if (!read_small(path, text, sizeof(text)))
        {
            verdict = PCT_NO_PROC;
            break;
        }
        os64_snprintf(line, sizeof(line), "\naffinity\t%s\n", want ? want : "any");
        size_t ll = os64_strlen(line);
        bool found = false;
        for (const char *p = text; *p && !found; p++)
            found = os64_memcmp(p, line, ll) == 0;
        if (!found)
        {
            os64_printf("pincputest: thread %s does not say 'affinity %s'\n%s",
                        e.name, want ? want : "any", text);
            verdict = PCT_BAD_RECORD;
        }
        seen++;
    }
    os64_close((int32_t)d);
    if (verdict == 0 && seen < 2)
        verdict = PCT_NO_PROC;
    return verdict;
}

// A second thread that parks until released, so /proc lists it while the
// records are read.
static volatile int gRelease;

static int64_t sample_then_wait(void *arg)
{
    int64_t r = sample(arg);
    while (!__atomic_load_n(&gRelease, __ATOMIC_ACQUIRE))
        os64_sleep(1);
    return r;
}

static int64_t idle_until_released(void *arg)
{
    (void)arg;
    while (!__atomic_load_n(&gRelease, __ATOMIC_ACQUIRE))
        os64_sleep(1);
    return 0;
}

static uint32_t child_pinned(const char *cpu_text)
{
    uint64_t cpu;
    uint32_t apic;
    if (!os64_parse_u64(cpu_text, &cpu))
        return PCT_BAD_RECORD;
    if (!apic_of_cpu(cpu, &apic))
        return PCT_NO_APIC;
    int64_t h = os64_thread(sample_then_wait, (void *)(uintptr_t)apic);
    if (h < 0)
        return PCT_NO_THREAD;
    uint32_t verdict = (uint32_t)sample((void *)(uintptr_t)apic);
    if (verdict == 0)
        verdict = check_records(cpu_text);
    __atomic_store_n(&gRelease, 1, __ATOMIC_RELEASE);
    int64_t theirs = 0;
    os64_thread_join((int32_t)h, &theirs);
    if (verdict == 0 && theirs != 0)
        verdict = (uint32_t)theirs;
    return verdict;
}

static uint32_t child_unpinned(void)
{
    int64_t h = os64_thread(idle_until_released, NULL);
    if (h < 0)
        return PCT_NO_THREAD;
    uint32_t verdict = check_records(NULL);
    __atomic_store_n(&gRelease, 1, __ATOMIC_RELEASE);
    os64_thread_join((int32_t)h, NULL);
    return verdict == PCT_BAD_RECORD ? PCT_REFUSAL_PINNED : verdict;
}

// Spawn this program with PINCPU and PINCORE set as given (NULL = unset);
// answer its code.
static uint32_t run_child(const char *pincpu, const char *pincore, char *const *argv)
{
    if ((pincpu && os64_setenv("PINCPU", pincpu) != 0) ||
        (pincore && os64_setenv("PINCORE", pincore) != 0))
        return PCT_NO_SETENV;
    int64_t pid = os64_spawn("/tests/pincputest", argv);
    os64_unsetenv("PINCPU");
    os64_unsetenv("PINCORE");
    if (pid < 0)
        return PCT_NO_SPAWN;
    int32_t code = PCT_NO_SPAWN;
    while (os64_wait(pid, &code) == OS64_INTERRUPTED)
        ;
    return (uint32_t)code;
}

static int report(const char *what, uint32_t code)
{
    os64_printf("pincputest: %s failed 0x%x\n", what, code);
    return (int)code;
}

int main(int argc, char **argv)
{
    if (argc == 3 && os64_streq(argv[1], "pinned"))
        return child_pinned(argv[2]);
    if (argc == 2 && os64_streq(argv[1], "unpinned"))
        return child_unpinned();

    char text[32];
    uint64_t cpus;
    if (!read_small("/sys/cpu/count", text, sizeof(text)))
        return PCT_NO_COUNT;
    for (char *p = text; *p; p++)
        if (*p == '\n')
            *p = '\0';
    if (!os64_parse_u64(text, &cpus) || cpus == 0)
        return PCT_NO_COUNT;

    char what[64];
    for (uint64_t c = 0; c < cpus; c++)
    {
        char cpu[24];
        os64_snprintf(cpu, sizeof(cpu), "%lu", c);
        char *child[] = { "/tests/pincputest", "pinned", cpu, NULL };
        uint32_t code = run_child(cpu, NULL, child);
        if (code != 0)
        {
            os64_snprintf(what, sizeof(what), "PINCPU=%s", cpu);
            return report(what, code);
        }
    }

    // The old name, on the LAST CPU — the one whose number and APIC ID
    // differ most on a sparse machine — and PINCPU outranking it.
    char last[24];
    os64_snprintf(last, sizeof(last), "%lu", cpus - 1);
    char *pinned_last[] = { "/tests/pincputest", "pinned", last, NULL };
    uint32_t code = run_child(NULL, last, pinned_last);
    if (code != 0)
    {
        os64_snprintf(what, sizeof(what), "PINCORE=%s", last);
        return report(what, code);
    }
    code = run_child(last, "x1", pinned_last);
    if (code != 0)
    {
        os64_snprintf(what, sizeof(what), "PINCPU=%s over PINCORE=x1", last);
        return report(what, code);
    }

    char absent[24];
    os64_snprintf(absent, sizeof(absent), "%lu", cpus);   // one past the last CPU
    const char *refused[] = { "x1", "1x", absent, "12345" };
    char *child[] = { "/tests/pincputest", "unpinned", NULL };
    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++)
    {
        code = run_child(refused[i], NULL, child);
        if (code != 0)
        {
            os64_snprintf(what, sizeof(what), "PINCPU=%s", refused[i]);
            return report(what, code);
        }
    }
    os64_printf("pincputest: pinned to each of %lu CPUs; PINCORE honoured; 4 refusals left unpinned\n", cpus);
    return PCT_OK;
}
