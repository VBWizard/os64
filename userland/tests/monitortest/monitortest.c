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
static void core_diagnostic(const char *label, const os64_monitor_snapshot_t *s)
{
    if (s->summary_valid) return;
    os64_printf("%s: partial=%u cores=%u interval=%lu tick=%lu ledger=%lu\n",
                label, s->partial, (uint32_t)s->core_count, s->interval_us,
                s->tick_interval_us, s->ledger_interval_us);
    for (size_t i = 0; i < s->core_count; i++)
        os64_printf("  core %u valid=%u total=%lu delta=%lu\n", s->cores[i].id,
                    s->cores[i].valid, s->cores[i].time.total,
                    s->cores[i].delta.total);
}

static void cpu_split_behavior(void)
{
    uint64_t pid = os64_taskid();
    os64_proc_info_t info;
    os64_proc_cpu_split_t before, spun, slept;
    if (os64_proc_read_split(pid, &info, &before) != 0 || !before.valid) {
        check(false, "initial task CPU split");
        return;
    }
    os64_ticks_t start, now;
    if (os64_ticks(&start) != 0 || !start.per_second) {
        check(false, "spin clock");
        return;
    }
    volatile uint64_t furnace = 0;
    do {
        for (unsigned i = 0; i < 1u << 18; i++) furnace += i;
        if (os64_ticks(&now) != 0) break;
    } while (now.ticks - start.ticks < start.per_second / 4);
    (void)furnace;
    if (os64_proc_read_split(pid, &info, &spun) != 0 || !spun.valid) {
        check(false, "spun task CPU split");
        return;
    }
    check(spun.user_us > before.user_us + 50000,
          "user spin charges user time");
    os64_sleep(300);
    if (os64_proc_read_split(pid, &info, &slept) != 0 || !slept.valid) {
        check(false, "slept task CPU split");
        return;
    }
    check(slept.user_us >= spun.user_us && slept.system_us >= spun.system_us,
          "split counters monotonic");
    check(slept.user_us - spun.user_us + slept.system_us - spun.system_us < 200000,
          "blocked sleep does not charge CPU time");
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
    core_diagnostic("A", sa);
    if (sa->summary_valid) {
        check(sa->machine_percent[0] + sa->machine_percent[1] + sa->machine_percent[2] == 100,
              "machine percentages close");
        os64_monitor_core_split_t machine = {0};
        check(os64_monitor_machine_split(a, &machine) && machine.valid,
              "machine user/system interval");
        if (machine.valid)
            check(machine.percent[0] + machine.percent[1] + machine.percent[2] +
                  machine.percent[3] == 100, "split machine percentages close");
    }
    for (size_t i = 0; i < sa->task_count; i++)
        if (sa->tasks[i].info.pid == os64_taskid()) {
            check(sa->tasks[i].cpu.valid, "self CPU measurement");
            os64_monitor_split_t split;
            check(os64_monitor_task_split(a, os64_taskid(), &split) &&
                  split.user.valid && split.system.valid, "self CPU split");
        }
    for (size_t i = 0; i < sa->thread_count; i++)
        if (sa->threads[i].info.pid == os64_taskid())
            check(sa->threads[i].cpu.valid, "live thread CPU interval");
    os64_sleep(300);
    check(os64_monitor_sample(b, false, &sb) == 0, "second B sample");
    if (sb) {
        check(sb->summary_valid && sb->ticks.ticks > held_ticks, "B independent interval");
        core_diagnostic("B", sb);
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
    cpu_split_behavior();
    os64_monitor_destroy(a); os64_monitor_destroy(b);
    os64_printf("monitortest: %s (%u failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
