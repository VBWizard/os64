// keytest.c — a throwaway app to prove read(0) works end to end.
//
// It blocks on os64_read(0, …), which travels: keyboard IRQ → keyboard.c
// translated-key ring → console_read (sleeps until a key arrives, woken via
// processSignals) → the read syscall → here, at CPL 3. Each key is echoed to
// the console AND reported to the serial log (so a headless run can verify it
// by injecting keys through the QEMU monitor). 'q' quits.
//
// Remove once husk exists — husk is the real consumer of read(0).

#include "os64/os64.h"

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    os64_debug_log("keytest: reading stdin (send q to quit)");

    for (;;) {
        char c;
        long n = os64_read(0, &c, 1);   // BLOCKS until a key is available
        if (n != 1)
            continue;

        // Echo to the console (what a terminal does).
        os64_write(1, &c, 1);

        // Report to serial so a headless run can see it. Patch the char into a
        // fixed message (index 13 is the placeholder).
        char msg[] = "keytest key: X";
        msg[13] = c;
        os64_debug_log(msg);

        if (c == 'q')
            break;
    }

    os64_debug_log("keytest: done, exiting 0");
    return 0;
}
