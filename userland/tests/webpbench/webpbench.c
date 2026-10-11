// Guest-only profile: isolated decoder copies, fixed input, independent pixels.
#include "os64/os64.h"
#include "../../libwebp/port/bench.h"

#ifdef WEBP_BENCH_SCALAR
#define MODE "scalar"
#else
#define MODE "sse2"
#endif
#define WORKERS 6
typedef struct {
    const char *name;
    const uint8_t *data;
    size_t length;
    uint32_t size, input_crc, pixel_crc;
} fixture_t;
#include "fixtures.h"

typedef struct {
    webp_bench_profile_t profile;
    int64_t call_us, verify_us, output_free_us;
    os64_webp_status_t status;
    uint32_t crc;
    bool valid;
} sample_t;
static uint32_t crc_table[256];
static int32_t report = OS64_STDOUT;
static bool write_failed;
static unsigned ready, start;
static sample_t concurrent[WORKERS];
static const fixture_t *work_fixture;

// A table keeps checking 64 MiB of pixels from dominating the untimed checks.
static uint32_t crc32(const void *data, size_t length)
{
    const uint8_t *p = data;
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < length; ++i)
        crc = crc_table[(crc ^ p[i]) & 255] ^ (crc >> 8);
    return crc ^ 0xffffffffu;
}

#define RECORD(...) do { if (os64_hprintf(report, __VA_ARGS__) < 0) write_failed = true; } while (0)

static sample_t measure(const fixture_t *f)
{
    sample_t s = {0};
    os64_webp_image_t image = {0};
    int64_t begin = os64_micros();
    s.status = webp_bench_decode(f->data, f->length, 0, 0, &image, &s.profile);
    int64_t decoded = os64_micros();
    s.call_us = decoded - begin;
    if (s.status == OS64_WEBP_OK && image.width == f->size && image.height == f->size) {
        s.crc = crc32(image.pixels, (size_t)image.width * image.height * 4);
        s.valid = s.crc == f->pixel_crc;
    }
    int64_t checked = os64_micros();
    s.verify_us = checked - decoded;
    os64_webp_free(&image);
    s.output_free_us = os64_micros() - checked;
    return s;
}

static void print_sample(const char *phase, const fixture_t *f, unsigned index, sample_t s)
{
    // The remainder still includes page faults, scheduling, fills and copies;
    // it is deliberately not called pure decoder CPU time.
    int64_t other = s.call_us - s.profile.gate_us - s.profile.allocate_us - s.profile.scratch_free_us;
    RECORD("sample mode=%s phase=%s fixture=%s index=%u call_us=%ld gate_us=%ld allocate_us=%ld scratch_free_us=%ld other_decode_us=%ld verify_us=%ld output_free_us=%ld crc=%08x valid=%d status=%s\n",
           MODE, phase, f->name, index, s.call_us, s.profile.gate_us,
           s.profile.allocate_us, s.profile.scratch_free_us, other,
           s.verify_us, s.output_free_us, s.crc, s.valid, os64_webp_status_name(s.status));
}

static int64_t worker(void *arg)
{
    unsigned i = (unsigned)(uintptr_t)arg;
    __atomic_fetch_add(&ready, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&start, __ATOMIC_ACQUIRE)) os64_sleep(1);
    concurrent[i] = measure(work_fixture);
    return concurrent[i].valid ? 0 : 1;
}

static bool contention(const fixture_t *f)
{
    work_fixture = f;
    ready = start = 0;
    int64_t handles[WORKERS];
    unsigned launched = 0;
    for (; launched < WORKERS; ++launched) {
        handles[launched] = os64_thread(worker, (void *)(uintptr_t)launched);
        if (handles[launched] < 0) break;
    }
    bool ok = launched == WORKERS;
    while (__atomic_load_n(&ready, __ATOMIC_ACQUIRE) != launched) os64_sleep(1);
    os64_proc_info_t before, after;
    bool cpu_before = os64_proc_read(os64_taskid(), &before) == 0;
    int64_t begin = os64_micros();
    __atomic_store_n(&start, 1, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < launched; ++i) {
        int64_t code = -1;
        if (os64_thread_join((int32_t)handles[i], &code) < 0 || code) ok = false;
        os64_close((int32_t)handles[i]);
    }
    int64_t elapsed = os64_micros() - begin;
    bool cpu_after = os64_proc_read(os64_taskid(), &after) == 0;
    bool cpu_valid = cpu_before && cpu_after && after.runtime_us >= before.runtime_us;
    for (unsigned i = 0; i < launched; ++i) print_sample("contended", f, i, concurrent[i]);
    RECORD("contention mode=%s fixture=%s workers=%u elapsed_us=%ld process_cpu_us=%lu cpu_valid=%d result=%d\n",
           MODE, f->name, launched, elapsed,
           cpu_valid ? after.runtime_us - before.runtime_us : 0, cpu_valid, ok ? 0 : 1);
    return ok && cpu_valid;
}

static void metadata(void)
{
    uint32_t a, b, c, d;
    char brand[49] = {0};
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000000u), "c"(0));
    if (a >= 0x80000004u) {
        for (uint32_t leaf = 0; leaf < 3; ++leaf) {
            uint32_t words[4];
            __asm__ volatile("cpuid" : "=a"(words[0]), "=b"(words[1]), "=c"(words[2]), "=d"(words[3]) : "a"(0x80000002u + leaf), "c"(0));
            os64_memcpy(brand + leaf * 16, words, 16);
        }
    }
    char count[32] = "unknown";
    int64_t fd = os64_open("/sys/cpu/count", "r");
    if (fd >= 0) {
        int64_t n = os64_read((int32_t)fd, count, sizeof(count)-1);
        if (n > 0) { count[n] = 0; if (count[n-1] == '\n') count[n-1] = 0; }
        os64_close((int32_t)fd);
    }
    RECORD("build mode=%s libwebp=1.6.0 compiler=%s built=%s_%s cpu=%s cores=%s\n",
           MODE, __VERSION__, __DATE__, __TIME__, brand[0] ? brand : "unknown", count);
}

int main(int argc, char **argv)
{
    bool quick = argc == 2 && os64_streq(argv[1], "--quick");
    if (argc != 1 && !quick) {
        os64_hprintf(OS64_STDERR, "usage: %s [--quick]\n", argv[0]);
        return 2;
    }
#ifndef WEBP_BENCH_SCALAR
    report = (int32_t)os64_open("/home/webp-bench.txt", "w");
    if (report < 0) {
        os64_hprintf(OS64_STDERR, "webpbench: cannot write /home/webp-bench.txt\n");
        return 3;
    }
    RECORD("webpbench version=1 scope=%s instrumented=yes\n", quick ? "quick" : "full");
#endif
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t n = i;
        for (unsigned bit = 0; bit < 8; ++bit) n = (n >> 1) ^ (0xedb88320u & (0u - (n & 1)));
        crc_table[i] = n;
    }
    bool ok = crc32("123456789", 9) == 0xcbf43926u;
    metadata();
    // Warm up DSP initialization with a small image, retaining the evidence
    // separately so first-call work is not mixed into the steady samples.
    sample_t warm = measure(&fixtures[0]);
    print_sample("warmup", &fixtures[0], 0, warm);
    ok = ok && warm.valid;
    for (unsigned i = 0; i < sizeof(fixtures)/sizeof(fixtures[0]) && ok; ++i) {
        const fixture_t *f = &fixtures[i];
        if (quick && f->size > 512) continue;
        uint32_t input_crc = crc32(f->data, f->length);
        RECORD("fixture mode=%s name=%s width=%u height=%u input_bytes=%lu input_crc=%08x expected_input_crc=%08x expected_pixel_crc=%08x\n",
               MODE, f->name, f->size, f->size, f->length, input_crc, f->input_crc, f->pixel_crc);
        if (input_crc != f->input_crc) { ok = false; break; }
        os64_hprintf(OS64_STDERR, "webpbench: %s %s\n", MODE, f->name);
        unsigned rounds = f->size == 512 ? 3 : 1;
        for (unsigned r = 0; r < rounds && ok; ++r) {
            sample_t s = measure(f);
            print_sample("serial", f, r, s);
            ok = s.valid;
        }
        os64_sync_all();
    }
    if (ok) {
        os64_hprintf(OS64_STDERR, "webpbench: %s six competing decodes\n", MODE);
        ok = contention(&fixtures[quick ? 2 : 5]);
    }
    uint64_t problems = os64_heap_verify();
    ok = ok && !problems;
    RECORD("mode_result mode=%s heap_problems=%lu result=%d\n", MODE, problems, ok ? 0 : 1);
#ifndef WEBP_BENCH_SCALAR
    if (ok && !write_failed) {
        char *args[] = {"webpbenchscalar", quick ? "--quick" : NULL, NULL};
        int64_t pid = os64_spawn_redirected("/tests/webpbenchscalar", args, -1, report, -1, 0);
        int32_t code = -1;
        int64_t waited = -1;
        if (pid >= 0) {
            do { waited = os64_wait(pid, &code); } while (waited == OS64_INTERRUPTED);
        }
        ok = pid >= 0 && waited >= 0 && code == 0;
        RECORD("scalar_child pid=%ld wait=%ld code=%d\n", pid, waited, code);
    }
    ok = ok && !write_failed;
    RECORD("complete scope=%s result=%d\n", quick ? "quick" : "full", ok ? 0 : 1);
    if (os64_close(report) < 0) ok = false;
    os64_hprintf(OS64_STDERR, "webpbench: %s; report /home/webp-bench.txt\n", ok && !write_failed ? "PASS" : "FAILED");
#endif
    os64_sync_all();
    return ok && !write_failed ? 0 : 1;
}
