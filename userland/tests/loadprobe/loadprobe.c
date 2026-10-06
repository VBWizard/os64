// P5 testing instrumentation: time shared operations while Yonder loads
// pictures. Start before navigation so launching this probe is not itself
// part of the observed external-command delay. No kernel behavior changes.
#include "os64/os64.h"
#include "os64/monitor.h"

static unsigned char buffer[65536];

static uint64_t elapsed(int64_t start)
{
    int64_t end = os64_micros();
    return start >= 0 && end >= start ? (uint64_t)(end - start) / 1000 : 0;
}

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;
    os64_monitor_t *monitor = os64_monitor_create(512, 2048, 24);
    if (!monitor) return 1;
    os64_memset(buffer, 0, sizeof(buffer));
    os64_printf("loadprobe: milliseconds; q quits between samples\n");
    for (;;) {
        char key;
        if (os64_read_for(OS64_STDIN, &key, 1, 0) == 1 && key == 'q') break;
        int64_t start = os64_micros();
        int64_t file = os64_open("/bin/ls", NULL);
        uint64_t open_ms = elapsed(start);
        start = os64_micros();
        int64_t bytes = file >= 0 ? os64_read((int32_t)file, buffer, sizeof(buffer)) : -1;
        uint64_t read_ms = elapsed(start);
        start = os64_micros();
        if (file >= 0) os64_close((int32_t)file);
        uint64_t close_ms = elapsed(start);
        os64_printf("file: open=%lu read=%lu close=%lu bytes=%ld\n",
                    open_ms, read_ms, close_ms, bytes);

        char *child_argv[] = {"/bin/true", NULL};
        start = os64_micros();
        int64_t child = os64_spawn(child_argv[0], child_argv);
        uint64_t spawn_ms = elapsed(start);
        start = os64_micros();
        int32_t exit_code = -1;
        int64_t wait_status = child >= 0 ? os64_wait(child, &exit_code) : -1;
        uint64_t wait_ms = elapsed(start);
        os64_printf("process: spawn=%lu wait=%lu pid=%ld status=%ld exit=%d\n",
                    spawn_ms, wait_ms, child, wait_status, exit_code);

        start = os64_micros();
        void *allocation = os64_malloc(1u << 20);
        if (allocation) os64_memset(allocation, 0, 1u << 20);
        uint64_t alloc_ms = elapsed(start);
        start = os64_micros();
        os64_free(allocation);
        uint64_t free_ms = elapsed(start);
        os64_memory_t memory;
        start = os64_micros();
        int64_t memory_status = os64_memory(&memory);
        uint64_t memory_ms = elapsed(start);
        os64_printf("memory: alloc+touch=%lu free=%lu snapshot=%lu status=%ld\n",
                    alloc_ms, free_ms, memory_ms, memory_status);

        const os64_monitor_snapshot_t *snapshot;
        start = os64_micros();
        int32_t task_status = os64_monitor_sample(monitor, false, &snapshot);
        uint64_t tasks_ms = elapsed(start);
        os64_printf("monitor: without-threads=%lu status=%d\n", tasks_ms, task_status);
        start = os64_micros();
        int32_t thread_status = os64_monitor_sample(monitor, true, &snapshot);
        uint64_t threads_ms = elapsed(start);
        os64_printf("monitor: with-threads=%lu status=%d\n", threads_ms, thread_status);
        os64_sleep(3000);
    }
    os64_monitor_destroy(monitor);
    return 0;
}
