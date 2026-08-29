// spin_test — the unkillable program, or so it thinks.
//
// A pure ring-3 spin: after the greeting it never makes another syscall, so
// the SIGINT "pull" path (dispatcher check + blocking-loop sentinels) can
// never touch it. It exists to prove the "push" path: the scheduler sees the
// pending SIGINT on a ring-3 frame and forces the thread into the exit
// trampoline — os32's forced-syscall trick, reborn in scheduler.c as
// scheduler_sigint_forced_syscall(). If Ctrl+C ends this program with $? =
// 130, the push works. If you're staring at a wedged console instead, it
// doesn't. (Test fixture, not a utility — nobody needs a spin(1).)

#include "os64/os64.h"

int main(void)
{
	os64_puts("spin_test: spinning forever, zero syscalls from here on — Ctrl+C me\n");
	for (;;)
	{
		// Nothing. Not even a yield — a yield would be a syscall, and the
		// whole point is to be the program the pull path can't reach.
	}
	return 0;   // unreachable; the only exit is the trampoline we get shoved into
}
