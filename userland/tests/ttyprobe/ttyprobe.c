// ttyprobe.c — the acceptance fixture for os64_tty_handle().
//
// The claim being tested is one sentence: a program whose handle 0 is a PIPE
// can still reach the keys typed at its terminal. That is the whole reason
// the call exists (`ps | less` — document from the pipe, keys from the
// terminal), and it is not a claim you can check by reading code, because
// every interesting part of it happens across the ring 0/3 boundary while a
// human leans on a key.
//
// Shaped after ptyprobe (PTY.md's "acceptance is the fixture, not the app"):
// prove the SYSCALL from a plain text VT, with no pager in sight, so that
// whoever writes the pager is debugging the pager. Three acts:
//
//   1. drain handle 0 to EOF and report the byte count — proves stdin really
//      was redirected, so act 3 cannot pass by accident on a terminal stdin
//   2. ask for a terminal handle and report which slot came back
//   3. read keys through it, patiently, and report each one
//
// Every report goes out os64_serial_log, NOT the console: a headless harness
// reads the wire, and this fixture's entire job is to be seen from outside.
//
// Run it as:  echo whatever | ttyprobe
// Then type. Three keys (or 20 seconds) and it leaves.

#include "os64/os64.h"
#include <stdarg.h>

#define PROBE_KEYS_WANTED   3
#define PROBE_PATIENCE_MS   20000
#define PROBE_SLICE_MS      1000    // one report per quiet second, so a
                                    // watcher can see it is alive and waiting

static void say(const char *fmt, ...)
{
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    os64_vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    os64_serial_log(line);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    say("TTYPROBE: act 1 - draining handle 0 to EOF");

    // Act 1. If husk redirected us, this is a pipe and it ends. If it is the
    // terminal, this blocks until Ctrl+D — which is itself a useful thing to
    // see, and the reason the count is reported rather than assumed.
    char buf[256];
    uint64_t total = 0;
    int64_t n;
    while ((n = os64_read(OS64_STDIN, buf, sizeof(buf))) > 0)
        total += (uint64_t)n;
    say("TTYPROBE: stdin drained, %lu bytes, read returned %ld (0 = EOF)",
        total, (long)n);

    // Act 2. The call under test. Note there is nothing to name and nothing
    // to open — the terminal is a property of THIS task, and the kernel
    // resolves which one at every read.
    int64_t keys = os64_tty_handle();
    if (keys < 0)
    {
        say("TTYPROBE: FAIL - os64_tty_handle returned %ld", (long)keys);
        return 1;
    }
    if (keys < 3)
    {
        // handle_alloc searches from slot 3 precisely so this cannot happen;
        // if it ever does, a fixture saying so beats a program silently
        // redirecting its own stdin.
        say("TTYPROBE: FAIL - handle %ld collides with a standard stream",
            (long)keys);
        return 1;
    }
    say("TTYPROBE: act 2 - tty handle is %ld", (long)keys);

    // Act 3. Keys, through a handle that is not stdin, while stdin is drained
    // and finished. On a VT these come from the keyboard; inside a terminal
    // window they come from the pty master. The fixture cannot tell, which is
    // the point.
    say("TTYPROBE: act 3 - type %d keys (waiting up to %d ms)",
        PROBE_KEYS_WANTED, PROBE_PATIENCE_MS);

    int got = 0;
    uint32_t waited = 0;
    while (got < PROBE_KEYS_WANTED && waited < PROBE_PATIENCE_MS)
    {
        char c;
        int64_t r = os64_read_for((int32_t)keys, &c, 1, PROBE_SLICE_MS);
        if (r == OS64_ERR_TIMEOUT)
        {
            waited += PROBE_SLICE_MS;
            say("TTYPROBE: waiting (%u ms)", waited);
            continue;
        }
        if (r <= 0)
        {
            say("TTYPROBE: read ended, returned %ld (0 = Ctrl+D)", (long)r);
            break;
        }
        got++;
        say("TTYPROBE: key %d = 0x%02x '%c'", got, (unsigned char)c,
            (c >= ' ' && c <= '~') ? c : '.');
    }

    os64_close((int32_t)keys);
    say("TTYPROBE: %s - %d of %d keys arrived through handle %ld",
        got >= PROBE_KEYS_WANTED ? "PASS" : "INCOMPLETE",
        got, PROBE_KEYS_WANTED, (long)keys);
    return got >= PROBE_KEYS_WANTED ? 0 : 1;
}
