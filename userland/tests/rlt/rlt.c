// rlt — THROWAWAY test fixture for os64_readline (delete freely; it exists
// so one QEMU boot can prove the line reader against real handles: a /proc
// file when given a path, stdin — possibly a pipe — when not).
//
// Every line goes to the framebuffer numbered, AND to the serial log via
// os64_debug_log, so a headless read of qemu_com1.log can verify the result.

#include "os64/os64.h"

int main(int argc, char **argv)
{
    char line[256];
    int64_t h = OS64_STDIN;
    int64_t r;
    int lineNo = 0;

    if (argc > 1)
    {
        h = os64_open(argv[1], NULL);
        if (h < 0)
        {
            os64_hprintf(OS64_STDERR, "rlt: cannot open %s\n", argv[1]);
            return 1;
        }
    }

    while ((r = os64_readline((int32_t)h, line, sizeof line)) == 1)
    {
        lineNo++;
        os64_printf("%d: %s\n", lineNo, line);
        os64_debug_log(line);
    }

    if (r < 0)
    {
        os64_hprintf(OS64_STDERR, "rlt: read error %d\n", (int32_t)r);
        return 1;
    }

    os64_printf("rlt: %d line(s)\n", lineNo);
    if (h != OS64_STDIN)
        os64_close((int32_t)h);
    return 0;
}
