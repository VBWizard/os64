// husk.c — the os64 shell. A husk is the outer shell of a seed; this is the
// outer shell of the kernel. It does NOTHING a real program can do: it reads a
// line, finds the program, spawns it, waits for it, and prompts again. ls,
// grep, cat — all separate executables husk spawns. Zero feature duplication.
//
// v1: read -> parse argv -> spawn -> wait -> repeat. One builtin: `exit`.
// v2: PIPELINES. `a | b | c` — husk creates the pipes, hands one end to each
// child, and then closes its own copies. Note that husk does not move a single
// byte of the data: it builds the plumbing and gets out of the way. The two
// programs never learn they aren't talking to a terminal.
// v3: REDIRECTION. `< file` and `> file` — husk opens the file and installs
// the handle in the child's slot 0 or 1, exactly the way it installs a pipe
// end. Same indirection, same close discipline, zero new mechanism in any
// child program: `upper < notes.txt > SHOUTING.txt` works because upper
// still just reads 0 and writes 1, none the wiser. A redirection beats a
// pipe when both claim the same slot (`a | b < f`: b reads f, and a's pipe
// simply sees its reader vanish — which is what SIGPIPE is for).

#include "os64/os64.h"

#define LINE_MAX 256
#define ARGS_MAX 16
#define MAX_STAGES 4          // a | b | c | d is plenty of rope for now

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

// Does the first whitespace-delimited word of `s` equal `word`? Non-destructive
// — unlike parse(), which tokenizes in place — so builtins can be recognized
// BEFORE the line is chewed up.
static int first_token_is(const char *s, const char *word)
{
	while (*s == ' ') s++;
	while (*word && *s == *word) { s++; word++; }
	return *word == 0 && (*s == 0 || *s == ' ');
}

// Split a line on '|' into stage strings, in place. Returns the stage count.
// (No quoting, no escapes — a bare '|' is always a pipe. Quoting is a parser
// feature and husk's parser is deliberately tiny.)
static int split_pipeline(char *line, char *stages[], int maxstages)
{
	int n = 0;
	char *p = line;

	stages[n++] = p;
	while (*p && n < maxstages)
	{
		if (*p == '|')
		{
			*p++ = 0;              // terminate the stage before the bar
			stages[n++] = p;       // next stage starts after it
		}
		else
		{
			p++;
		}
	}
	return n;
}

// Report a reaped child to the serial log: "husk: pid <p> exited code <c>".
static void report_exit(long ended, int code)
{
	char msg[64];
	char nb[20];
	int m = 0;

	for (const char *s = "husk: pid "; *s; s++) msg[m++] = *s;
	{ int k = utoa((unsigned long)ended, nb); for (int j = 0; j < k; j++) msg[m++] = nb[j]; }
	for (const char *s = " exited code "; *s; s++) msg[m++] = *s;
	{ int k = utoa((unsigned long)(unsigned int)code, nb); for (int j = 0; j < k; j++) msg[m++] = nb[j]; }
	msg[m] = 0;
	os64_debug_log(msg);
}

// Pull `< file` / `> file` out of an already-parsed argv, compacting what
// remains. Writes the filenames through the out-params (NULL = no redirect)
// and returns 0, or -1 on a dangling operator (`upper <` with no filename).
// The operators must be their own tokens — husk's parser splits on spaces
// only, and that simplicity is a feature (`upper<f` is a program named
// "upper<f", which is honest, if unhelpful).
static int extract_redirections(char *cargv[], char **inFile, char **outFile)
{
	*inFile = NULL;
	*outFile = NULL;

	int w = 0;
	for (int r = 0; cargv[r]; r++)
	{
		if (str_eq(cargv[r], "<"))
		{
			if (!cargv[r + 1]) return -1;
			*inFile = cargv[++r];
		}
		else if (str_eq(cargv[r], ">"))
		{
			if (!cargv[r + 1]) return -1;
			*outFile = cargv[++r];
		}
		else
		{
			cargv[w++] = cargv[r];
		}
	}
	cargv[w] = 0;
	return w == 0 ? -1 : 0;   // a line that was ALL redirections has no program
}

// Build and run a pipeline: spawn every stage, wiring stage i's stdout to
// stage i+1's stdin through a pipe. The last stage keeps the console.
//
// THE CLOSE DISCIPLINE IS THE WHOLE JOB. Every end husk hands to a child, husk
// must then close its OWN copy of — because the reader downstream sees
// end-of-input only when the LAST write end closes. Keep husk's copy of a write
// end open and that reader waits forever for an EOF that can never come: the
// classic `a | b` hang that every hand-written shell suffers exactly once. The
// child already holds its own reference, so closing ours takes nothing from it.
static void run_pipeline(char *stages[], int nstages)
{
	long pids[MAX_STAGES];
	int npids = 0;
	int prev_read = -1;         // read end of the pipe from the PREVIOUS stage

	for (int i = 0; i < nstages; i++)
	{
		char *cargv[ARGS_MAX];
		if (parse(stages[i], cargv, ARGS_MAX) == 0)
		{
			os64_puts("husk: empty command in pipeline\n");
			break;
		}

		// Redirections come out of argv before the child ever sees it —
		// `upper < in > out` runs upper with argc == 1, exactly as if the
		// shell had been reading and writing the files itself.
		char *inFile, *outFile;
		if (extract_redirections(cargv, &inFile, &outFile) < 0)
		{
			os64_puts("husk: bad redirection (expected `< file` or `> file`)\n");
			break;
		}

		// Open redirect files BEFORE creating the pipe — if the file isn't
		// there, we want to fail while there's nothing yet to unwind.
		int inRedir = -1, outRedir = -1;
		if (inFile && (inRedir = (int)os64_open(inFile, "r")) < 0)
		{
			os64_puts("husk: cannot open ");
			os64_puts(inFile);
			os64_puts("\n");
			break;
		}
		if (outFile && (outRedir = (int)os64_open(outFile, "w")) < 0)
		{
			os64_puts("husk: cannot create ");
			os64_puts(outFile);
			os64_puts("\n");
			if (inRedir >= 0) os64_close(inRedir);
			break;
		}

		// A pipe to the NEXT stage — the last stage doesn't need one.
		int p[2] = { -1, -1 };
		if (i < nstages - 1 && os64_pipe(p) < 0)
		{
			os64_puts("husk: out of pipes\n");
			if (inRedir >= 0)  os64_close(inRedir);
			if (outRedir >= 0) os64_close(outRedir);
			break;
		}

		// Slot priority: an explicit redirect beats the pipeline's plumbing.
		int in  = (inRedir  >= 0) ? inRedir  : prev_read;    // -1: console
		int out = (outRedir >= 0) ? outRedir
		        : (i < nstages - 1) ? p[1] : -1;             // -1: console

		long pid = os64_spawn_redirected(cargv[0], cargv, in, out, -1);

		// Hand-off done — drop husk's copies of EVERYTHING it just passed
		// along (or displaced). The displaced case matters: if a redirect won
		// slot 0 over prev_read, husk still holds the pipe's read end, and
		// closing it is what tells the upstream writer its reader is gone.
		if (prev_read >= 0) { os64_close(prev_read); prev_read = -1; }
		if (p[1] >= 0)        os64_close(p[1]);
		if (inRedir >= 0)     os64_close(inRedir);
		if (outRedir >= 0)    os64_close(outRedir);

		if (pid < 0)
		{
			os64_puts("husk: cannot run ");
			os64_puts(cargv[0]);
			os64_puts("\n");
			if (p[0] >= 0) os64_close(p[0]);
			break;
		}

		pids[npids++] = pid;
		prev_read = p[0];       // this stage's output becomes the next one's input
	}

	if (prev_read >= 0)
		os64_close(prev_read);  // belt and braces: never leave an end dangling

	// Reap every child we launched. They run CONCURRENTLY — that is the point
	// of a pipeline; stage 2 is already chewing on stage 1's first bytes long
	// before stage 1 finishes. We just collect the corpses in order.
	for (int i = 0; i < npids; i++)
	{
		int code = 0;
		long ended = os64_wait(pids[i], &code);
		report_exit(ended, code);
	}
}

// ── the shell ───────────────────────────────────────────────────────────────

int main(int argc, char **argv, char **envp)
{
	(void)argc; (void)argv; (void)envp;

	os64_write(1, "husk — the os64 shell. `exit` to quit.\n",
	           sizeof("husk — the os64 shell. `exit` to quit.\n") - 1);
	os64_debug_log("husk: started");

	char line[LINE_MAX];
	char *stages[MAX_STAGES];

	for (;;)
	{
		prompt();
		if (read_line(line, sizeof(line)) == 0)
			continue;

		int nstages = split_pipeline(line, stages, MAX_STAGES);

		// `exit` is a builtin because it touches the SHELL'S OWN state (its
		// lifetime) — no separate program could ever do it. Checked WITHOUT
		// parse(), which tokenizes in place and would eat the line before
		// run_pipeline ever saw the arguments.
		if (nstages == 1 && first_token_is(stages[0], "exit"))
			break;

		// `cd` is THE canonical builtin — the textbook answer to "why must
		// any command be built in?": an external cd would change ITS OWN
		// cwd (a copy inherited at spawn) and take the change to its grave.
		// Only the shell can move the shell. `cd` alone goes to the root —
		// there's no $HOME to go home to yet.
		if (nstages == 1 && first_token_is(stages[0], "cd"))
		{
			char *cargv[ARGS_MAX];
			int cargc = parse(stages[0], cargv, ARGS_MAX);
			const char *dest = (cargc > 1) ? cargv[1] : "/";
			if (os64_chdir(dest) < 0)
			{
				os64_puts("husk: cd: no such directory: ");
				os64_puts(dest);
				os64_puts("\n");
			}
			continue;
		}

		run_pipeline(stages, nstages);
	}

	os64_write(1, "husk: bye\n", 10);
	os64_debug_log("husk: exiting");
	return 0;
}
