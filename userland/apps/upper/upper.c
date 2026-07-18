// upper.c — THE TEMPLATE FILTER. Read stdin, transform, write stdout.
//
// This is the shape every os64 utility takes, and it is deliberately the
// smallest complete example of it. `ls`, `cat`, `grep`, `wc`, `sort` — they all
// differ only in what happens between the read and the write. Steal this file.
//
// Three things make it a filter, and all three matter:
//
//   1. It reads handle 0 and writes handle 1 and NEVER ASKS WHAT THEY ARE.
//      Run it alone and 0 is the keyboard, 1 is the console. Run it in a
//      pipeline and the shell has quietly pointed them at pipes. This program
//      cannot tell the difference and contains ZERO lines of pipe-awareness.
//      That indirection is the entire reason small programs compose.
//
//   2. It loops until read returns 0. A short read is normal — you get what is
//      available right now, not a filled buffer — so a filter processes what it
//      gets and comes back for more. Zero means end of input: every writer on
//      the other end has closed. It is not a byte in the stream, it is their
//      absence.
//
//   3. It is STREAMING. It never holds the whole input in memory — one buffer,
//      reused. That is what lets `bigfile | upper | grep x` run in constant
//      memory no matter how big the file is, and it is why a pipe's fixed
//      capacity (which blocks a fast writer until the reader catches up) is a
//      feature and not a limit.
//
// Usage:  upper            (type, see it shouted back)
//         hello | upper    (the first os64 pipeline)
//
// LONG RUN: upper will take the cat model — args are filenames, no args means
// stdin — once file handles/open() land (deferred past the shell, per the
// roadmap). Until then it REFUSES args instead of silently ignoring them:
// `upper hello` blocking forever on the keyboard while you stare at it is a
// 20-minute debugging session waiting to happen (ask us how we know).

#include "os64/os64.h"

#define BUF_SIZE 512

int main(int argc, char **argv, char **envp)
{
	(void)argv; (void)envp;

	// The tripwire: fail LOUDLY on args we can't honor yet, on handle 2 so a
	// pipeline's data stream stays clean. A filter that ignores its arguments
	// and blocks on the keyboard isn't broken — it's worse: it's *waiting*.
	// When open() exists, this branch becomes the for-each-filename loop.
	if (argc > 1)
	{
		static const char usage[] =
			"upper: file arguments not supported yet (no open() until file "
			"handles land)\nusage: upper < input, or pipe into it\n";
		os64_write(2, usage, sizeof(usage) - 1);
		return 1;
	}

	char buf[BUF_SIZE];
	long n;

	// The canonical filter loop. Every utility you write will have this spine.
	while ((n = os64_read(0, buf, sizeof(buf))) > 0)
	{
		// The only interesting line in the program: the transform.
		for (long i = 0; i < n; i++)
			if (buf[i] >= 'a' && buf[i] <= 'z')
				buf[i] = (char)(buf[i] - 'a' + 'A');

		// Write what we transformed — exactly n bytes, no more, no less. If the
		// consumer downstream is slow, this BLOCKS until there is room, which
		// throttles us to its speed. We sleep; we do not spin.
		os64_write(1, buf, (size_t)n);
	}

	// n == 0: end of input, the clean exit. (n < 0 would be a real error.)
	return n < 0 ? 1 : 0;
}
