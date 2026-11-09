// rngprobe surveys CPU RNG instructions. Bounded samples can expose missing
// or stuck facilities; they do not establish entropy quality or source trust.

#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include "os64/fmt.h"
#include "os64/io.h"

#define SAMPLE_COUNT 256u
#define ATTEMPTS_PER_SAMPLE 1024u

typedef void (*cpuid_fn)(uint32_t leaf, uint32_t subleaf, uint32_t out[4]);
typedef bool (*sample_fn)(void *context, uint64_t *value);

typedef struct {
    char vendor[13];
    char brand[49];
    uint32_t signature;
    bool hypervisor;
    bool rdrand;
    bool rdseed;
} identity_t;

typedef struct {
    uint32_t successes;
    uint32_t attempts;
    uint32_t exhausted;
    uint32_t max_attempts;
    uint32_t adjacent_repeats;
    uint32_t zeros;
    uint32_t ones;
} samples_t;

static void report(const char *format, ...)
{
    char line[256];
    va_list args;
    va_start(args, format);
    os64_vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    os64_printf("%s\n", line);
    os64_serial_log(line);
}

static void cpu_id(uint32_t leaf, uint32_t subleaf, uint32_t out[4])
{
    __asm__ volatile("cpuid"
                     : "=a"(out[0]), "=b"(out[1]), "=c"(out[2]), "=d"(out[3])
                     : "a"(leaf), "c"(subleaf));
}

static void copy_word(char *out, uint32_t word)
{
    for (unsigned i = 0; i < 4; i++) {
        unsigned byte = (word >> (i * 8)) & 255u;
        // CPU strings belong in a one-line report, even under a hypervisor
        // that supplies control characters in its identification leaves.
        out[i] = byte == 0 ? ' ' : byte >= 32 && byte <= 126 ? (char)byte : '?';
    }
}

static identity_t identify(cpuid_fn query)
{
    identity_t cpu = {0};
    uint32_t regs[4];
    query(0, 0, regs);
    uint32_t maximum = regs[0];
    copy_word(cpu.vendor, regs[1]);
    copy_word(cpu.vendor + 4, regs[3]);
    copy_word(cpu.vendor + 8, regs[2]);
    if (maximum >= 1) {
        query(1, 0, regs);
        cpu.signature = regs[0];
        cpu.hypervisor = (regs[2] & (1u << 31)) != 0;
        cpu.rdrand = (regs[2] & (1u << 30)) != 0;
    }
    if (maximum >= 7) {
        query(7, 0, regs);
        cpu.rdseed = (regs[1] & (1u << 18)) != 0;
    }
    query(0x80000000u, 0, regs);
    if (regs[0] >= 0x80000004u) {
        for (unsigned leaf = 0; leaf < 3; leaf++) {
            query(0x80000002u + leaf, 0, regs);
            for (unsigned word = 0; word < 4; word++)
                copy_word(cpu.brand + leaf * 16 + word * 4, regs[word]);
        }
    }
    return cpu;
}

static bool read_rdrand(void *context, uint64_t *value)
{
    (void)context;
    unsigned char ready;
    __asm__ volatile("rdrand %0; setc %1"
                     : "=&r"(*value), "=qm"(ready) : : "cc");
    return ready != 0;
}

static bool read_rdseed(void *context, uint64_t *value)
{
    (void)context;
    unsigned char ready;
    __asm__ volatile("rdseed %0; setc %1"
                     : "=&r"(*value), "=qm"(ready) : : "cc");
    return ready != 0;
}

static samples_t collect(sample_fn next, void *context)
{
    samples_t samples = {0};
    uint64_t previous = 0;
    for (unsigned slot = 0; slot < SAMPLE_COUNT; slot++) {
        bool ready = false;
        unsigned attempts = 0;
        uint64_t value = 0;
        while (attempts < ATTEMPTS_PER_SAMPLE) {
            attempts++;
            samples.attempts++;
            if (next(context, &value)) {
                ready = true;
                break;
            }
            __asm__ volatile("pause");
        }
        if (attempts > samples.max_attempts)
            samples.max_attempts = attempts;
        if (!ready) {
            samples.exhausted++;
            continue; // A failed instruction's destination is not a sample.
        }
        if (samples.successes && value == previous)
            samples.adjacent_repeats++;
        samples.zeros += value == 0;
        samples.ones += value == UINT64_MAX;
        previous = value;
        samples.successes++;
    }
    return samples;
}

static bool stuck(const samples_t *samples)
{
    return samples->successes > 1 &&
           samples->adjacent_repeats == samples->successes - 1;
}

static bool probe(const char *name, bool supported, sample_fn next)
{
    if (!supported) {
        report("rngprobe: %s unsupported; instruction not executed", name);
        return true;
    }
    // Print before executing so an advertised-but-faulting instruction is
    // identifiable from the report even when the OS terminates this task.
    report("rngprobe: %s sampling %u words; retry cap=%u per word",
           name, SAMPLE_COUNT, ATTEMPTS_PER_SAMPLE);
    samples_t samples = collect(next, NULL);
    report("rngprobe: %s successes=%u attempts=%u exhausted=%u max_attempts=%u",
           name, samples.successes, samples.attempts, samples.exhausted,
           samples.max_attempts);
    report("rngprobe: %s adjacent_repeats=%u zeros=%u all_ones=%u",
           name, samples.adjacent_repeats, samples.zeros, samples.ones);
    bool passed = samples.exhausted == 0 && !stuck(&samples);
    report("rngprobe: %s %s", name,
           stuck(&samples) ? "SUSPICIOUS: successful output was constant" :
           samples.exhausted ? "INCOMPLETE: retry budget exhausted" :
           "sampling complete; no constant-output failure observed");
    return passed;
}

int main(void)
{
    identity_t cpu = identify(cpu_id);
    report("rngprobe: vendor=%s signature=0x%08x hypervisor=%s",
           cpu.vendor, cpu.signature, cpu.hypervisor ? "yes" : "no");
    report("rngprobe: brand=%s", cpu.brand[0] ? cpu.brand : "not reported");
    report("rngprobe: CPUID rdrand=%s rdseed=%s",
           cpu.rdrand ? "yes" : "no", cpu.rdseed ? "yes" : "no");
    bool rdrand_ok = probe("RDRAND", cpu.rdrand, read_rdrand);
    bool rdseed_ok = probe("RDSEED", cpu.rdseed, read_rdseed);
    int result = !rdrand_ok || !rdseed_ok ? 1 :
                 !cpu.rdrand && !cpu.rdseed ? 3 : 0;
    report("rngprobe: result=%s; availability survey, not entropy certification",
           result == 0 ? "COMPLETE" : result == 3 ? "UNSUPPORTED" : "CHECK_FAILED");
    return result;
}
