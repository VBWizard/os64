// sigdemo — watch a program catch Ctrl+C and keep going.
//
// The FIXTURE (/tests/sigtest) proves signal handling is correct and says so in
// an exit code, which is the right shape for a test suite and a terrible show.
// This is the other thing: a countdown you interrupt with your own hands, that
// tells you it was interrupted and then carries on counting.
//
// Chris's os32 test app is the direct ancestor — countdown, visible
// interruption, visible recovery — and the reason it exists is that "it
// returned 85327872" is a verdict, not a demonstration.
//
//   sigdemo [seconds]      default 20
//
// Press Ctrl+C while it counts. Without a handler that would end the program
// on the spot (SIGINT's default action is death, exit 130). With one, the
// kernel runs the handler, the sleep it interrupted reports OS64_INTERRUPTED,
// and the countdown resumes — which is the whole of what SIGNALS.md step 3
// bought.
//
// Three Ctrl+Cs and it gives in, which is a courtesy rather than a rule: a
// program that catches SIGINT forever is a program you can only kill, and
// making the demo behave the way a well-mannered program should is part of
// showing what handlers are FOR.

#include <stdint.h>
#include <stdbool.h>

#include "os64/os64.h"
#include "os64/signal.h"

#define GIVE_IN_AFTER 3

// `volatile` because the handler writes it and the loop reads it, and nothing
// in between tells the compiler that can happen. This is the one piece of
// discipline every signal-handling program needs and most learn the hard way.
static volatile int gInterrupts;
static volatile int gLastSignal;

static void on_interrupt(int signo)
{
    gLastSignal = signo;
    gInterrupts++;
}

int main(int argc, char **argv)
{
    int64_t seconds = 20;
    if (argc > 1)
    {
        int64_t n = os64_atoi(argv[1]);
        if (n > 0)
            seconds = n;
    }

    if (os64_signal_set_handler(OS64_SIGINT, on_interrupt) < 0)
    {
        os64_hprintf(OS64_STDERR, "sigdemo: this kernel cannot install a handler\n");
        return 1;
    }

    os64_printf("sigdemo: counting down from %ld. Press Ctrl+C.\n", (long)seconds);
    os64_printf("         (without a handler that would kill me on the spot)\n\n");

    int seen = 0;
    for (int64_t left = seconds; left > 0; left--)
    {
        os64_printf("  %ld...\n", (long)left);

        // One second, in chunks, so an interrupt is noticed promptly and the
        // countdown stays honest about how much time actually passed. A sleep
        // cut short does NOT resume — os64 has no SA_RESTART — so the loop is
        // where the retry lives, which is exactly the point of returning
        // OS64_INTERRUPTED instead of pretending.
        int64_t remaining_ms = 1000;
        while (remaining_ms > 0)
        {
            int64_t slept = os64_sleep(remaining_ms > 100 ? 100 : remaining_ms);
            if (slept == OS64_INTERRUPTED)
            {
                // The handler already ran — by the time this line executes,
                // gInterrupts has been incremented from inside it.
                if (gInterrupts > seen)
                {
                    seen = gInterrupts;
                    os64_printf("\n  *** caught signal %d — and I am still here (%d of %d) ***\n\n",
                                gLastSignal, seen, GIVE_IN_AFTER);
                    if (seen >= GIVE_IN_AFTER)
                    {
                        os64_printf("  all right, all right. Exiting politely.\n");
                        return 0;
                    }
                }
                continue;   // the nap was cut short; finish it
            }
            remaining_ms -= 100;
        }
    }

    os64_printf("\nsigdemo: countdown finished");
    if (gInterrupts > 0)
        os64_printf(" — survived %d interrupt%s", gInterrupts,
                    gInterrupts == 1 ? "" : "s");
    os64_printf(".\n");
    return 0;
}
