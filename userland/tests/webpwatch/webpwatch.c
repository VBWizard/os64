// Observe a real Yonder process during navigation/close stress. Raw heap
// snapshots retain the seqlock's torn marker; analysis must exclude torn reads.
#include "os64/os64.h"

static void physical(const char *phase)
{
    os64_memory_t m;
    if (os64_memory(&m) == 0)
        os64_printf("physical %s used=%lu available=%lu\n", phase, m.used, m.available);
}

static void heap(uint64_t pid)
{
    char path[80], bytes[4096];
    os64_snprintf(path, sizeof(path), "/proc/%lu/heap", pid);
    int64_t fd = os64_open(path, "r");
    if (fd < 0) return;
    int64_t n;
    while ((n = os64_read((int32_t)fd, bytes, sizeof(bytes))) > 0)
        if (os64_write(OS64_STDOUT, bytes, (size_t)n) != n) break;
    os64_close((int32_t)fd);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        os64_printf("usage: webpwatch URL > /home/webp-watch.txt\n"
                    "Close Yonder normally when finished; ten minutes is the failure deadline.\n");
        return 2;
    }
    physical("before");
    char *args[] = {"yonder", argv[1], NULL};
    int64_t start = os64_micros();
    int64_t pid = os64_spawn("/bin/yonder", args);
    if (pid < 0) return 3;
    os64_printf("watch pid=%ld start_us=%ld\n", pid, start);
    int32_t code = -1;
    bool timed_out = false;
    for (;;) {
        int64_t reaped = os64_reap(&code);
        if (reaped == pid) break;
        if (reaped < 0) return 4;
        int64_t elapsed = os64_micros() - start;
        if (elapsed > 600000000) {
            // A deadline is a failed run, never evidence of graceful close.
            char path[80];
            os64_snprintf(path, sizeof(path), "/proc/%ld/ctl", pid);
            int64_t fd = os64_open(path, "w");
            if (fd >= 0) {
                os64_write((int32_t)fd, "kill", 4);
                os64_close((int32_t)fd);
            }
            timed_out = true;
            os64_wait(pid, &code);
            break;
        }
        os64_proc_info_t info;
        if (os64_proc_read((uint64_t)pid, &info) == 0) {
            os64_printf("sample elapsed_us=%ld cpu_us=%lu threads=%u\n",
                        elapsed, info.runtime_us, info.threads);
            heap((uint64_t)pid);
            os64_printf("end-sample\n");
        }
        os64_sleep(200);
    }
    os64_printf("exit elapsed_us=%ld code=%d timeout=%d\n",
                os64_micros() - start, code, timed_out);
    os64_sleep(1000);
    physical("after");
    os64_sync_all();
    return timed_out || code != 0 ? 1 : 0;
}
