// patest.c — printat() syscall fixture. NOT a clock (the clock is somebody
// else's joy); this proves the widget plane end to end so clock lands on
// verified rails.
//
// What it exercises, in order:
//   1. A visible marker parked at a fixed cell while the console keeps its
//      cursor — printat must not move the prompt.
//   2. Rapid repaints at one cell (the high-refresh case clock exists for).
//   3. Edge clipping: a string parked near the right edge must truncate,
//      never wrap onto the next console line.
//   4. Boundary rejection: absurd coordinates and a wild string pointer must
//      come back as errors, not as scribbles (or worse).
//
// Verdict goes to BOTH sinks: the console (human watching the window) and
// the serial log via os64_debug_log (me, reading the scratchpad log).

#include "os64/os64.h"
#include "os64/fmt.h"

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    int failures = 0;

    // 1+2: repaint a counter at row 2, right side — 20 frames at ~10Hz.
    // (Row 0 belongs to ktask's clock; a fixture doesn't pick that fight.)
    for (int i = 0; i < 20; i++) {
        char line[32];
        os64_snprintf(line, sizeof line, "[patest %d]", i);
        if (os64_screen_printat(95, 2, line) != 0)
            failures++;
        os64_sleep(100);
    }

    // 3: park a long string 8 cells from wherever the right edge is —
    // if it wraps or reflows the console, the eyeball test fails loudly.
    if (os64_screen_printat(120, 3, "EDGE-CLIP-TEST-SHOULD-TRUNCATE") != 0)
        failures++;

    // 4: the boundary must say no. Both calls should return negative
    // in-band statuses, and the OS should not so much as flinch.
    if (os64_screen_printat(100000, 0, "never") == 0) {
        os64_puts("patest: FAIL — absurd x accepted\n");
        failures++;
    }
    if (os64_screen_printat(0, 5, (const char *)0xdeadbeef000) == 0) {
        os64_puts("patest: FAIL — wild pointer accepted\n");
        failures++;
    }

    if (failures == 0) {
        os64_puts("patest: PASS — widget plane verified\n");
        os64_debug_log("patest: PASS");
    } else {
        os64_puts("patest: FAILURES — see above\n");
        os64_debug_log("patest: FAIL");
    }

    return failures == 0 ? 0 : 1;
}
