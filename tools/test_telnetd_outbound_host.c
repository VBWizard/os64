// Include the real outbound loop; deterministic reads and mailbox timing
// exercise its CR lookahead, final drain and short socket writes.
#define main telnetd_program_main
#include "../userland/apps/telnetd/telnetd.c"
#undef main
#include <assert.h>
#include <string.h>
#include <stdio.h>

static unsigned step, scenario;
static uint8_t wire[4096];
static size_t wire_len;
static const uint8_t notice[] = "NOTICE\r\n";

int64_t os64_write(int32_t handle, const void *buf, size_t len)
{
    assert(handle == g_conn && wire_len + len < sizeof(wire));
    size_t n = len > 1 ? 1 : len; // force write_all's short-write loop
    memcpy(wire + wire_len, buf, n);
    wire_len += n;
    return (int64_t)n;
}

int64_t os64_read_for(int32_t handle, void *buf, size_t len, uint64_t ms)
{
    assert(handle == g_master && len >= 2 && ms == 100 && step < 4);
    if (step++ == 0)
    {
        memcpy(buf, "x\r", 2);
        if (scenario == 5)
            g_end_after_drain = 1; // shutdown with an already empty mailbox
        if (scenario < 2)
        {
            assert(mbox_put(notice, sizeof(notice)-1) == sizeof(notice)-1);
            if (scenario == 0)
                g_end_after_drain = 1;
        }
        return 2;
    }
    if (scenario == 1)
        g_end_after_drain = 1; // notice was drained before the end flag
    if (scenario == 2 && step == 2)
        return OS64_ERR_TIMEOUT;
    if (scenario == 3 && step == 2)
    {
        memcpy(buf, "\ny", 2);
        return 2;
    }
    return 0;
}

int main(void)
{
    for (scenario = 0; scenario < 6; scenario++)
    {
        step=0; wire_len=0; g_mbox_head=g_mbox_tail=0;
        g_session_over=0;g_end_after_drain=0;g_master=3;
        outbound_thread(NULL);
        const char *expected = scenario < 2 ? "x\r\0NOTICE\r\n" :
                               scenario == 3 ? "x\r\ny" : "x\r\0";
        size_t expected_len = scenario < 2 ? 11 : scenario == 3 ? 4 : 3;
        assert(g_session_over && wire_len == expected_len);
        assert(memcmp(wire, expected, expected_len) == 0);
    }
    puts("telnetd outbound: CR before closing notice, delayed end flag, idle/EOF CR and split CRLF PASS");
}
