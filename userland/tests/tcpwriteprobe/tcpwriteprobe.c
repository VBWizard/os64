// The host withholds reads until told to resume, then verifies the stream.
#include "os64/os64.h"

static uint8_t pattern(size_t at) { return (uint8_t)(((uint32_t)at * 2654435761u) >> 24); }
static uint64_t ticks(void)
{
    os64_ticks_t clock;
    if (os64_ticks(&clock) < 0) return 0;
    return clock.ticks;
}
int main(int argc, char **argv)
{
    if (argc != 3) { os64_printf("usage: tcpwriteprobe HOST PORT (tools/test_tcp_write_peer.py)\n"); return 1; }
    char target[128];
    os64_snprintf(target, sizeof target, "tcp!%s!%s", argv[1], argv[2]);
    int64_t control = os64_dial(target);
    if (control < 0) return 2;
    int64_t data = os64_dial(target);
    if (data < 0) { os64_close((int32_t)control); return 2; }
    bool pass = false; unsigned char bytes[4096], ready;
    size_t total = 0;
    if (os64_read_for((int32_t)control, &ready, 1, 5000) != 1 || ready != 'R') goto done;
    if (os64_write_for(OS64_STDOUT, NULL, 0, 0) != -1 ||
        os64_write_for((int32_t)data, NULL, 0, 0) != 0) goto done;
    os64_ticks_t clock;
    if (os64_ticks(&clock) < 0 || !clock.per_second) goto done;
    uint64_t start = clock.ticks, before = 0, after = 0;
    bool timeout = false;
    while (total < 16u * 1024u * 1024u && ticks() - start < 10u * clock.per_second) {
        for (size_t i = 0; i < sizeof bytes; i++) bytes[i] = pattern(total + i);
        // Poll first; once full, require a finite empty wait without closing.
        int64_t n = os64_write_for((int32_t)data, bytes, sizeof bytes, 0);
        if (n == OS64_ERR_TIMEOUT) {
            before = ticks();
            n = os64_write_for((int32_t)data, bytes, sizeof bytes, 100);
            after = ticks();
            if (n == OS64_ERR_TIMEOUT) { timeout = true; break; }
        }
        if (n <= 0 || n > (int64_t)sizeof bytes) goto done;
        total += (size_t)n;
    }
    if (!timeout || (after - before) * 1000u < 100u * clock.per_second ||
        after - before > 2u * clock.per_second) goto done;
    size_t goal = total + 32768;
    char message[64];
    int count = os64_snprintf(message, sizeof message, "resume %lu\n", (unsigned long)goal);
    if (os64_write((int32_t)control, message, (size_t)count) != count) goto done;
    start = ticks();
    while (total < goal && ticks() - start < 30u * clock.per_second) {
        size_t length = goal - total; if (length > sizeof bytes) length = sizeof bytes;
        for (size_t i = 0; i < length; i++) bytes[i] = pattern(total + i);
        int64_t n = os64_write_for((int32_t)data, bytes, length, 1000);
        if (n == OS64_ERR_TIMEOUT) continue;
        if (n <= 0 || (size_t)n > length) goto done;
        total += (size_t)n;
    }
    if (total != goal) goto done;
    os64_close((int32_t)data); data = -1;
    if (os64_read_for((int32_t)control, &ready, 1, 30000) != 1 || ready != 'K') goto done;
    os64_printf("tcpwriteprobe: PASS poll, %lu-tick timeout, retry and %lu verified bytes\n",
                (unsigned long)(after - before), (unsigned long)total);
    os64_serial_log("TCPWRITE PASS timeout and exact stream after retry");
    pass = true;
done:
    if (data >= 0) os64_close((int32_t)data);
    os64_close((int32_t)control);
    if (!pass) { os64_printf("tcpwriteprobe: FAIL after %lu queued bytes\n", (unsigned long)total); os64_serial_log("TCPWRITE FAIL"); }
    return pass ? 0 : 3;
}
