// Exercise the production logging paths with CLS reads and sleeps forbidden.
// Run with tools/test_log_drain_host.sh. Real kernel headers preserve the ring
// layout; hardware and scheduler boundaries are replaced by host stubs.
#include "smp_core.h"

extern int puts(const char *);
extern _Noreturn void exit(int);

static _Noreturn void fail(const char *message)
{
    puts(message);
    exit(1);
}

static core_local_storage_t *forbidden_cls_read(void)
{
    fail("FAIL: synchronous logging read CLS");
}

#define get_core_local_storage forbidden_cls_read
#include "../kernel/src/logging/log.c"
#undef get_core_local_storage

volatile uint8_t kMPCoreCount = 1;
volatile uint64_t kTicksSinceStart = 1;
volatile uint64_t kSystemCurrentTime = 0;
char kLogdPath[128] = "";
bool kSerialPresent = true;
bool kSMPInitDone = false;

static unsigned serial_calls;
static log_entry_t entries[LOGD_DRAIN_CHUNK + 2];

void serial_print_string(const char *message)
{
    (void)message;
    serial_calls++;
}

uint32_t read_apic_id(void) { return 0; }

void panic(const char *format, ...)
{
    (void)format;
    fail("FAIL: unexpected kernel panic");
}

void signal_raise(signals signal, uint64_t data, void *thread)
{
    (void)signal; (void)data; (void)thread;
    fail("FAIL: synchronous logging attempted to sleep");
}

// The test uses boot-time clock state, before the RTC supplies an epoch.
// Trap an unexpected call rather than linking against libc's different ABI.
struct tm *gmtime(const time_t *timer, struct tm *out)
{
    (void)timer; (void)out;
    fail("FAIL: unexpected wall-clock conversion");
}

static void prepare(size_t count, size_t capacity)
{
    memset(entries, 0, sizeof(entries));
    core_log_buffers[0] = (log_buffer_t){
        .entries = entries, .head = count, .capacity = capacity,
    };
    for (size_t i = 0; i < count; i++) {
        entries[i].tsc = i;
        snprintf(entries[i].message, sizeof(entries[i].message), "entry %lu\n", (unsigned long)i);
    }
    kLoggingInitialized = true;
    serial_calls = 0;
}

static void check(bool condition, const char *message)
{
    if (!condition)
        fail(message);
}

int main(int argc, char **argv)
{
    if (argc != 2)
        fail("usage: test_log_drain_host empty|one|backlog|full|emergency|deferred");
    const size_t capacity = sizeof(entries) / sizeof(entries[0]);
    if (!strcmp(argv[1], "empty")) {
        prepare(0, capacity);
        check(!logd_thread(false), "FAIL: empty drain reported work");
        check(serial_calls == 0, "FAIL: empty drain emitted entries");
    } else if (!strcmp(argv[1], "one")) {
        prepare(1, capacity);
        check(logd_thread(false), "FAIL: single entry was not drained");
        check(serial_calls == 1 && core_log_buffers[0].tail == 1,
              "FAIL: single-entry drain did not consume and emit the entry");
    } else if (!strcmp(argv[1], "backlog")) {
        // Keep a backlog through the statistics interval, reaching the
        // formatter even when the chunk limit ends each drain pass.
        for (unsigned pass = 0; pass < LOGD_STATS_EVERY; pass++) {
            prepare(LOGD_DRAIN_CHUNK + 1, capacity);
            check(logd_thread(false), "FAIL: backlog drain reported no work");
            check(serial_calls == LOGD_DRAIN_CHUNK &&
                  core_log_buffers[0].tail == LOGD_DRAIN_CHUNK,
                  "FAIL: backlog drain did not respect its chunk bound");
        }
    } else if (!strcmp(argv[1], "full")) {
        prepare(2, 4);
        log_store_entry(0, 1, 0, 0, false, "fills the ring\n");
        check(serial_calls == 3 && core_log_buffers[0].head == core_log_buffers[0].tail,
              "FAIL: full-ring producer did not drain its entries");
        check(core_log_buffers[0].lost == 0, "FAIL: full-ring producer lost an entry");
    } else if (!strcmp(argv[1], "emergency")) {
        prepare(3, capacity);
        entries[1].continued = true;
        logd_emergency_flush();
        check(serial_calls == 3 && core_log_buffers[0].tail == 3,
              "FAIL: emergency flush did not consume its entries");
    } else if (!strcmp(argv[1], "deferred")) {
        prepare(1, capacity);
        kLogdPath[0] = '/';
        check(!logd_thread(false), "FAIL: deferred drain reported work");
        check(serial_calls == 0 && core_log_buffers[0].tail == 0,
              "FAIL: deferred drain consumed an entry");
    } else {
        fail("FAIL: unknown test case");
    }
    check(kLogDWorkLock == 0 && kLogDDrainerCore == UINT32_MAX,
          "FAIL: drain retained its lock or owner marker");
    puts("PASS");
    return 0;
}
