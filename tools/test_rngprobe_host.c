// Exercise feature gating and failure accounting without depending on a
// functioning hardware RNG. --live separately runs the real host probe.
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define main rngprobe_main
#include "../userland/tests/rngprobe/rngprobe.c"
#undef main

int32_t os64_vsnprintf(char *out, size_t size, const char *format, va_list args)
{
    return vsnprintf(out, size, format, args);
}

int32_t os64_printf(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int result = vprintf(format, args);
    va_end(args);
    return result;
}

void os64_serial_log(const char *line)
{
    (void)line;
}

static uint32_t basic_max;
static unsigned queries;

static void fake_cpuid(uint32_t leaf, uint32_t subleaf, uint32_t out[4])
{
    assert(subleaf == 0);
    memset(out, 0, 4 * sizeof(*out));
    queries++;
    switch (leaf) {
    case 0:
        out[0] = basic_max;
        out[1] = 0x0a414141u; // A control byte must not split a report line.
        break;
    case 1:
        assert(basic_max >= 1);
        out[0] = 0x12345678u;
        out[2] = (1u << 31) | (1u << 30);
        break;
    case 7:
        assert(basic_max >= 7);
        out[1] = 1u << 18;
        break;
    case 0x80000000u:
        break;
    default:
        assert(!"queried an unsupported CPUID leaf");
    }
}

typedef struct {
    uint32_t calls;
    uint32_t successes;
    uint32_t period;
    bool constant;
} sequence_t;

static bool sequence(void *context, uint64_t *value)
{
    sequence_t *s = context;
    s->calls++;
    *value = UINT64_MAX; // Deliberate junk when the carry flag says failure.
    if (!s->period || s->calls % s->period)
        return false;
    s->successes++;
    *value = s->constant ? UINT64_MAX : s->successes;
    return true;
}

static bool forbidden(void *context, uint64_t *value)
{
    (void)context;
    (void)value;
    assert(!"unsupported RNG instruction was executed");
    return false;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--live") == 0)
        return rngprobe_main();
    identity_t cpu;
    basic_max = 0;
    queries = 0;
    cpu = identify(fake_cpuid);
    assert(queries == 2 && !cpu.rdrand && !cpu.rdseed && !cpu.hypervisor);
    assert(cpu.vendor[3] == '?' && cpu.vendor[12] == 0 && cpu.brand[0] == 0);
    basic_max = 1;
    cpu = identify(fake_cpuid);
    assert(cpu.rdrand && cpu.hypervisor && !cpu.rdseed);
    basic_max = 7;
    cpu = identify(fake_cpuid);
    assert(cpu.rdrand && cpu.rdseed && cpu.signature == 0x12345678u);
    assert(probe("absent", false, forbidden));

    sequence_t s = {.period = 1};
    samples_t got = collect(sequence, &s);
    assert(got.successes == SAMPLE_COUNT && got.attempts == SAMPLE_COUNT);
    assert(got.max_attempts == 1 && got.exhausted == 0 && !stuck(&got));
    assert(got.adjacent_repeats == 0 && got.zeros == 0 && got.ones == 0);

    s = (sequence_t){.period = 3};
    got = collect(sequence, &s);
    assert(got.successes == SAMPLE_COUNT && got.attempts == 3 * SAMPLE_COUNT);
    assert(got.max_attempts == 3 && got.exhausted == 0 && got.ones == 0);

    s = (sequence_t){.period = ATTEMPTS_PER_SAMPLE};
    got = collect(sequence, &s);
    assert(got.successes == SAMPLE_COUNT && got.exhausted == 0);
    assert(got.attempts == SAMPLE_COUNT * ATTEMPTS_PER_SAMPLE);

    s = (sequence_t){0};
    got = collect(sequence, &s);
    assert(got.successes == 0 && got.exhausted == SAMPLE_COUNT && !stuck(&got));
    assert(got.max_attempts == ATTEMPTS_PER_SAMPLE);
    assert(s.calls == SAMPLE_COUNT * ATTEMPTS_PER_SAMPLE);
    assert(got.zeros == 0 && got.ones == 0 && got.adjacent_repeats == 0);

    s = (sequence_t){.period = ATTEMPTS_PER_SAMPLE + 1};
    got = collect(sequence, &s);
    assert(got.successes == SAMPLE_COUNT / 2 && got.exhausted == SAMPLE_COUNT / 2);
    assert(got.ones == 0 && got.adjacent_repeats == 0 && !stuck(&got));

    s = (sequence_t){.period = 1, .constant = true};
    got = collect(sequence, &s);
    assert(stuck(&got) && got.ones == SAMPLE_COUNT);
    assert(got.adjacent_repeats == SAMPLE_COUNT - 1);
    puts("rngprobe host checks: PASS");
    return 0;
}
