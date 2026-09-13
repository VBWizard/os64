// Public spawn_seated contract: STREAM EOF is terminal, GRID can re-seat.
#include "os64/os64.h"
#include "os64/pty.h"

#define PASS 0x57EA0000u

static int64_t seat(int64_t master)
{
    return os64_spawn_seated("/tests/streamseat",
                            (char *[]){"/tests/streamseat", "child", NULL}, master);
}

static bool wait_child(int64_t pid)
{
    int32_t code = -1;
    return pid > 0 && os64_wait(pid, &code) == pid && code == 0;
}

int main(int argc, char **argv)
{
    (void)argv;
    if (argc > 1)
    {
        os64_write(1, "child-output", 12);
        return 0;
    }
    int64_t master = os64_pty_create_stream(80, 24);
    if (master < 0)
        return PASS | 1;
    // A failed load must leave a fresh stream available for a first seat.
    int64_t bad = os64_spawn_seated("/no-such-streamseat-program", NULL, master);
    char buf[64];
    if (bad >= 0 || os64_read_for((int32_t)master, buf, sizeof(buf), 0) != OS64_ERR_TIMEOUT)
        return PASS | 2;
    if (!wait_child(seat(master)))
        return PASS | 3;
    int64_t bytes = 0, n;
    while ((n = os64_read_for((int32_t)master, buf, sizeof(buf), 1000)) > 0)
        bytes += n;
    if (n != 0 || bytes != 12)
        return PASS | 4;
    int64_t again = seat(master);
    if (again >= 0)
    {
        wait_child(again);
        os64_close((int32_t)master);
        os64_printf("streamseat: FAIL spawn accepted after STREAM EOF\n");
        return PASS | 5;
    }
    if (os64_read_for((int32_t)master, buf, sizeof(buf), 0) != 0)
        return PASS | 6;
    os64_close((int32_t)master);

    master = os64_pty_create(80, 24);
    if (master < 0 || !wait_child(seat(master)) || !wait_child(seat(master)))
        return PASS | 7;
    os64_close((int32_t)master);
    os64_printf("streamseat: PASS failed load preserves fresh stream, EOF refuses re-seat, GRID re-seats\n");
    return PASS;
}
