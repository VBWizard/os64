// husk.c — the os64 shell. A husk is the outer shell of a seed; this is the
// outer shell of the kernel. It does NOTHING a real program can do: it reads a
// line, finds the program, spawns it, waits for it, and prompts again. ls,
// grep, cat — all separate executables husk spawns. Zero feature duplication.
//
// v1: read -> parse argv -> spawn -> wait -> repeat. One builtin: `exit`.
// stdin/stdout are the console (handles 0/1); no pipes, no redirection, no
// TTYs yet — those layer on later without changing this loop.

#include "os64/os64.h"

#define LINE_MAX 256
#define ARGS_MAX 16

// ── tiny freestanding helpers (no libc) ─────────────────────────────────────

static int str_eq(const char *a, const char *b)
{
	while (*a && *a == *b) { a++; b++; }
	return *a == *b;
}

// Unsigned decimal to string. Returns length. For the diagnostic lines below.
static int utoa(unsigned long v, char *buf)
{
	char tmp[20];
	int i = 0;
	if (v == 0) { buf[0] = '0'; buf[1] = 0; return 1; }
	while (v) { tmp[i++] = (char)('0' + (v % 10)); v /= 10; }
	for (int j = 0; j < i; j++) buf[j] = tmp[i - 1 - j];
	buf[i] = 0;
	return i;
}

// ── line editing ────────────────────────────────────────────────────────────

static void prompt(void) { os64_write(1, "husk> ", 6); }

// Read one line from the console into buf (NUL-terminated), echoing as we go.
// Returns the length. Handles Enter (submit) and Backspace (erase).
static int read_line(char *buf, int cap)
{
	int n = 0;
	for (;;)
	{
		char c;
		if (os64_read(0, &c, 1) != 1)
			continue;

		if (c == '\r' || c == '\n')
		{
			os64_write(1, "\n", 1);
			buf[n] = 0;
			return n;
		}
		if (c == 0x08 || c == 0x7f)          // backspace / delete
		{
			if (n > 0) { n--; os64_write(1, "\b \b", 3); }   // rub out one glyph
			continue;
		}
		if (n < cap - 1)
		{
			buf[n++] = c;
			os64_write(1, &c, 1);            // echo
		}
	}
}

// Tokenize `line` in place on spaces into argv[]; NUL-terminates argv. argc.
static int parse(char *line, char *argv[], int maxargs)
{
	int argc = 0;
	char *p = line;
	while (*p && argc < maxargs - 1)
	{
		while (*p == ' ') p++;               // skip leading spaces
		if (!*p) break;
		argv[argc++] = p;                    // token starts here
		while (*p && *p != ' ') p++;         // to end of token
		if (*p) *p++ = 0;                    // terminate it
	}
	argv[argc] = 0;
	return argc;
}

// ── the shell ───────────────────────────────────────────────────────────────

int main(int argc, char **argv, char **envp)
{
	(void)argc; (void)argv; (void)envp;

	os64_write(1, "husk — the os64 shell. `exit` to quit.\n",
	           sizeof("husk — the os64 shell. `exit` to quit.\n") - 1);
	os64_debug_log("husk: started");

	char line[LINE_MAX];
	char *cargv[ARGS_MAX];

	for (;;)
	{
		prompt();
		int n = read_line(line, sizeof(line));
		if (n == 0)
			continue;

		int ac = parse(line, cargv, ARGS_MAX);
		if (ac == 0)
			continue;

		if (str_eq(cargv[0], "exit"))
			break;

		long pid = os64_spawn(cargv[0], cargv);
		if (pid < 0)
		{
			os64_puts("husk: cannot run ");
			os64_puts(cargv[0]);
			os64_puts("\n");
			os64_debug_log("husk: spawn failed");
			continue;
		}

		int code = 0;
		long ended = os64_wait(pid, &code);

		// Diagnostic to serial: proves spawn returned a pid and wait reaped it
		// with the child's exit code. Build "husk: pid <p> exited code <c>".
		char msg[64];
		int m = 0;
		char nb[20];
		for (const char *s = "husk: pid "; *s; s++) msg[m++] = *s;
		{ int k = utoa((unsigned long)ended, nb); for (int j = 0; j < k; j++) msg[m++] = nb[j]; }
		for (const char *s = " exited code "; *s; s++) msg[m++] = *s;
		{ int k = utoa((unsigned long)(unsigned int)code, nb); for (int j = 0; j < k; j++) msg[m++] = nb[j]; }
		msg[m] = 0;
		os64_debug_log(msg);
	}

	os64_write(1, "husk: bye\n", 10);
	os64_debug_log("husk: exiting");
	return 0;
}
