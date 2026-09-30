#include "os64/os64.h"
#include "os64/monitor.h"

static unsigned failures;
static bool worker_done;

static int64_t worker(void *unused)
{
    (void)unused;
    while (!__atomic_load_n(&worker_done, __ATOMIC_ACQUIRE)) os64_sleep(20);
    return 0;
}
static void check(bool yes, const char *what)
{
    if (!yes) { failures++; os64_printf("FAIL: %s\n", what); }
}

int main(void)
{
    os64_monitor_t *a = os64_monitor_create(512, 512, 32);
    os64_monitor_t *b = os64_monitor_create(512, 0, 32);
    if (!a || !b) {
        os64_monitor_destroy(a); os64_monitor_destroy(b);
        os64_puts("FAIL: monitor allocation\n"); return 1;
    }
    int64_t thread = os64_thread(worker, NULL);
    check(thread >= 0, "worker creation");
    const os64_monitor_snapshot_t *sa = NULL, *sb = NULL;
    check(os64_monitor_sample(a, true, &sa) == 0, "first A sample");
    check(os64_monitor_sample(b, false, &sb) == 0, "first B sample");
    if (!sa || !sb) goto done;
    check(sa->task_count > 0 && sa->core_count > 0 && sa->memory_valid, "live sources");
    check(!sa->partial && !sb->partial, "complete samples");
    check(!sa->summary_valid && !sb->summary_valid, "first sample is a baseline");
    bool found = false;
    for (size_t i = 0; i < sa->task_count; i++)
        if (sa->tasks[i].info.pid == os64_taskid()) {
            found = true; check(!sa->tasks[i].cpu.valid, "first task measurement");
        }
    check(found, "monitor sees itself");
    size_t own_threads = 0;
    for (size_t i = 0; i < sa->thread_count; i++)
        if (sa->threads[i].info.pid == os64_taskid()) own_threads++;
    check(own_threads >= 2, "live thread roster");
    uint64_t held_ticks = sb->ticks.ticks;
    os64_sleep(300);
    check(os64_monitor_sample(a, true, &sa) == 0, "second A sample");
    if (!sa) goto done;
    check(sb->sequence == 1 && sb->ticks.ticks == held_ticks, "B snapshot survives A sampling");
    check(sa->summary_valid && sa->interval_us > 0, "A CPU interval");
    if (sa->summary_valid)
        check(sa->machine_percent[0] + sa->machine_percent[1] + sa->machine_percent[2] == 100,
              "machine percentages close");
    for (size_t i = 0; i < sa->task_count; i++)
        if (sa->tasks[i].info.pid == os64_taskid()) check(sa->tasks[i].cpu.valid, "self CPU measurement");
    for (size_t i = 0; i < sa->thread_count; i++)
        if (sa->threads[i].info.pid == os64_taskid())
            check(sa->threads[i].cpu.valid, "live thread CPU interval");
    os64_sleep(300);
    check(os64_monitor_sample(b, false, &sb) == 0, "second B sample");
    if (sb) {
        check(sb->summary_valid && sb->ticks.ticks > held_ticks, "B independent interval");
        check(sb->tick_interval_us > sa->tick_interval_us, "B covers the longer interval");
        check(!sb->threads_sampled && !sb->thread_count, "optional threads");
    }
done:
    __atomic_store_n(&worker_done, true, __ATOMIC_RELEASE);
    if (thread >= 0) {
        int64_t answer = -1;
        check(os64_thread_join((int32_t)thread, &answer) == 0 && answer == 0, "worker join");
        os64_close((int32_t)thread);
    }
    os64_monitor_destroy(a); os64_monitor_destroy(b);
    os64_printf("monitortest: %s (%u failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
