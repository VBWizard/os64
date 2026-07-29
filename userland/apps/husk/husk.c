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
		if (c == 0x03)                       // ETX — Ctrl+C at the prompt
		{
			// The kernel only lets ETX through as DATA when the shell itself
			// is foreground (console_intr_intercept) — any other time it
			// becomes a SIGINT and we never see the byte. So this is always
			// "Ctrl+C at the prompt": kill the half-typed line, say so, and
			// let main() re-prompt. A keystroke that visibly does nothing
			// erodes all faith that it ever does anything — house doctrine.
			os64_write(1, "^C\n", 3);
			buf[0] = 0;
			return 0;
		}
		// Other control chords (Ctrl+A..Z now arrive as 0x01..0x1A) have no
		// line-editing meaning yet — swallow them rather than burying
		// invisible bytes in the command. Tab stays: it's typeable text.
		if ((unsigned char)c < 0x20 && c != '\t')
			continue;
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

// Expand $-variables in a command line. Expansion happens here, in the
// shell, before tokenization — so it works in front of every program ever
// written, not just the ones that opted in (os32 put expansion in the
// library and each app called it or didn't; echo did, most didn't).
//
// The vocabulary, in lookup order:
//   $?     the last exit status (arrived with the Bourne shell, 1977; the
//          Thompson shell could branch on a status but never let you SEE one)
//   $CWD   the current directory, fetched LIVE from the kernel at expansion
//          time. Unix's $PWD (csh's $cwd, 1978) is a shell-maintained COPY
//          of kernel state, patched by hand on every cd and famous for
//          drifting; os64 declines the cache and asks the owner — the truth
//          costs one syscall and can never be stale.
//   $NAME  the env block (os64_getenv). An unset name expands to nothing —
//          Bourne's rule; a literal "$NOPE" in the output helps nobody.
// A '$' that starts no name ($ alone, "$5", "$/") stays a literal '$'.
static int is_name_start(char c)
{
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}
static int is_name_char(char c)
{
	return is_name_start(c) || (c >= '0' && c <= '9');
}

static void expand_line(const char *src, char *dst, int cap, int last_status)
{
	int n = 0;
	while (*src && n < cap - 1)
	{
		if (src[0] == '$' && src[1] == '?')
		{
			char nb[20];
			int k = utoa((unsigned long)(unsigned int)last_status, nb);
			for (int j = 0; j < k && n < cap - 1; j++)
				dst[n++] = nb[j];
			src += 2;
		}
		else if (src[0] == '$' && is_name_start(src[1]))
		{
			char name[64];
			int k = 0;
			src++;                              // step over the '$'
			while (is_name_char(*src) && k < (int)sizeof(name) - 1)
				name[k++] = *src++;
			name[k] = 0;

			if (str_eq(name, "CWD"))
			{
				char cwd[256];
				if (os64_getcwd(cwd, sizeof(cwd)) >= 0)
					for (const char *v = cwd; *v && n < cap - 1; v++)
						dst[n++] = *v;
			}
			else
			{
				const char *v = os64_getenv(name);
				for (; v && *v && n < cap - 1; v++)
					dst[n++] = *v;
			}
		}
		else
			dst[n++] = *src++;
	}
	dst[n] = 0;
}

// ── background jobs ─────────────────────────────────────────────────────────
// `cmd &` — the Thompson shell grew this in 1973, and os32's kshell had it too
// (fork, skip the waitpid, never speak of the child again). os64 keeps the
// half that was right and fixes the half that wasn't: os32 never collected a
// backgrounded child's status, so every one of them became a zombie nobody
// ever buried. Here, REPORTING a finished job and REAPING it are the same act
// — os64_reap() at each prompt hands back a corpse, and the job table turns
// that into `[1]+ 57 Done`. Nothing accumulates, and the kernel's kworker
// never has to care: a shell buries its own dead.
#define MAX_JOBS 8

typedef struct {
	int  used;
	int  jobNum;                 // husk-local, small, what a human says out loud
	long pids[MAX_STAGES];       // every stage — `a | b &` is ONE job
	int  remaining;              // stages not yet collected
	long reportPid;              // the LAST stage's pid: the job's public identity
	int  lastCode;               // and its exit code — the pipeline's answer
} job_t;

static job_t gJobs[MAX_JOBS];
static int   gNextJobNum = 1;

static void put_num(unsigned long v)
{
	char b[20];
	int k = utoa(v, b);
	os64_write(1, b, (unsigned)k);
}

// "[1] 57" — job number and task number both, on purpose. In bash the pid is
// noise because you kill a job by `%1`; os64 has no such notation, so the TASK
// NUMBER is the handle you actually use: `echo kill > /proc/57/ctl`. Printing
// it is load-bearing here, not decoration.
static void job_announce(int jobNum, long pid)
{
	os64_write(1, "[", 1);
	put_num((unsigned long)jobNum);
	os64_write(1, "] ", 2);
	put_num((unsigned long)pid);
	os64_write(1, "\n", 1);
}

static void job_report_done(const job_t *j)
{
	os64_write(1, "[", 1);
	put_num((unsigned long)j->jobNum);
	os64_write(1, "]+ ", 3);
	put_num((unsigned long)j->reportPid);
	// Bourne's distinction, and a useful one: a job that FAILED should not read
	// the same as one that succeeded from across the room.
	if (j->lastCode == 0)
		os64_write(1, " Done\n", 6);
	else
	{
		os64_write(1, " Exit ", 6);
		put_num((unsigned long)(unsigned int)j->lastCode);
		os64_write(1, "\n", 1);
	}
}

// Register a launched background pipeline. Returns the job number, or 0 if the
// table is full (the job still RUNS — it just goes untracked, which is os32's
// behavior and an honest fallback rather than a refusal to launch).
static int job_add(const long *pids, int npids)
{
	for (int i = 0; i < MAX_JOBS; i++)
	{
		if (gJobs[i].used)
			continue;
		gJobs[i].used      = 1;
		gJobs[i].jobNum    = gNextJobNum++;
		gJobs[i].remaining = npids;
		gJobs[i].lastCode  = 0;
		for (int k = 0; k < npids; k++)
			gJobs[i].pids[k] = pids[k];
		gJobs[i].reportPid = pids[npids - 1];   // the last stage speaks for the job
		return gJobs[i].jobNum;
	}
	return 0;
}

// A child just came back from reap(). If it belongs to a tracked job, count it
// off; when the job's last stage is collected, announce it. Returns 1 if the
// pid was ours to account for.
static int job_note_reaped(long pid, int code)
{
	for (int i = 0; i < MAX_JOBS; i++)
	{
		if (!gJobs[i].used)
			continue;
		for (int k = 0; k < MAX_STAGES; k++)
		{
			if (gJobs[i].pids[k] != pid)
				continue;
			gJobs[i].pids[k] = -1;             // collected
			if (pid == gJobs[i].reportPid)
				gJobs[i].lastCode = code;      // the pipeline's status is its last stage's
			if (--gJobs[i].remaining == 0)
			{
				job_report_done(&gJobs[i]);
				gJobs[i].used = 0;
			}
			return 1;
		}
	}
	return 0;
}

// Collect every finished background child. Called at the prompt, which is the
// one moment a shell is guaranteed not to be in the middle of something — the
// same place bash reports jobs, and for the same reason.
static void jobs_poll(void)
{
	for (;;)
	{
		int code = 0;
		long pid = os64_reap(&code);
		if (pid <= 0)
			break;              // 0 = nobody has died; that is the usual answer
		job_note_reaped(pid, code);
	}
}

// Strip a trailing `&` and say whether it was there. The `&` must be its OWN
// token, exactly like `<` and `>`: husk splits on spaces only, so `hog&` is a
// program named "hog&" — honest, if unhelpful, and consistent with every other
// operator in this shell. (os32's kshell tested `*argv[argc-1]=='&'` and never
// removed it, so every background program there was handed a final argument of
// "&" and had to not care.)
static int strip_background(char *line)
{
	int n = 0;
	while (line[n] != '\0')
		n++;
	while (n > 0 && (line[n - 1] == ' ' || line[n - 1] == '\t'))
		n--;
	if (n == 0 || line[n - 1] != '&')
		return 0;
	if (n >= 2 && line[n - 2] != ' ' && line[n - 2] != '\t')
		return 0;               // part of a token, not an operator
	n--;                        // drop the '&' itself
	while (n > 0 && (line[n - 1] == ' ' || line[n - 1] == '\t'))
		n--;
	line[n] = '\0';
	return 1;
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

// PATH search — V7's gift (1979; before that Unix shells hardcoded /bin).
// Resolve a command name to something spawnable:
//   - a name containing '/' names a PLACE — used exactly as typed, no search
//   - a bare name tries the cwd first (so `cd /bin` + `ls` works exactly as
//     it did before PATH existed), then each colon-separated PATH directory
// Existence is probed with os64_stat — "what is this one name?" — so a
// directory can never win the search (typing `bin` at / must not try to
// exec a directory). Returns `cmd` itself or `buf` filled with the hit;
// on no hit, returns `cmd` unresolved and lets spawn deliver the "no".
static const char *resolve_command(const char *cmd, char *buf, int cap)
{
	for (const char *p = cmd; *p != '\0'; p++)
		if (*p == '/')
			return cmd;

	os64_dirent_t e;
	if (os64_stat(cmd, &e) == 0 && !(e.flags & OS64_DE_DIR))
		return cmd;

	const char *path = os64_getenv("PATH");
	if (path == NULL)
		return cmd;

	while (*path != '\0')
	{
		int n = 0;
		while (*path != '\0' && *path != ':' && n < cap - 1)
			buf[n++] = *path++;
		if (*path == ':')
			path++;                        // step over the separator
		if (n == 0)
			continue;                      // empty PATH element: skip
		if (n < cap - 1 && buf[n - 1] != '/')
			buf[n++] = '/';
		for (const char *c = cmd; *c != '\0' && n < cap - 1; c++)
			buf[n++] = *c;
		buf[n] = '\0';

		if (os64_stat(buf, &e) == 0 && !(e.flags & OS64_DE_DIR))
			return buf;
	}
	return cmd;
}

// Build and run a pipeline: spawn every stage, wiring stage i's stdout to
// stage i+1's stdin through a pipe. The last stage keeps the console.
//
// Returns the pipeline's exit status for $?: the LAST stage's exit code —
// the same answer the Bourne shell has given since 1977, and the sensible
// one: the last stage is the program whose output you just watched. Any
// husk-side failure (bad redirection, unspawnable program) reports 1.
//
// THE CLOSE DISCIPLINE IS THE WHOLE JOB. Every end husk hands to a child, husk
// must then close its OWN copy of — because the reader downstream sees
// end-of-input only when the LAST write end closes. Keep husk's copy of a write
// end open and that reader waits forever for an EOF that can never come: the
// classic `a | b` hang that every hand-written shell suffers exactly once. The
// child already holds its own reference, so closing ours takes nothing from it.
static int run_pipeline(char *stages[], int nstages, int background)
{
	long pids[MAX_STAGES];
	int npids = 0;
	int prev_read = -1;         // read end of the pipe from the PREVIOUS stage
	int status = 0;             // what $? will remember of this line

	for (int i = 0; i < nstages; i++)
	{
		char *cargv[ARGS_MAX];
		if (parse(stages[i], cargv, ARGS_MAX) == 0)
		{
			os64_puts("husk: empty command in pipeline\n");
			status = 1;
			break;
		}

		// Redirections come out of argv before the child ever sees it —
		// `upper < in > out` runs upper with argc == 1, exactly as if the
		// shell had been reading and writing the files itself.
		char *inFile, *outFile;
		if (extract_redirections(cargv, &inFile, &outFile) < 0)
		{
			os64_puts("husk: bad redirection (expected `< file` or `> file`)\n");
			status = 1;
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
			status = 1;
			break;
		}
		if (outFile && (outRedir = (int)os64_open(outFile, "w")) < 0)
		{
			os64_puts("husk: cannot create ");
			os64_puts(outFile);
			os64_puts("\n");
			if (inRedir >= 0) os64_close(inRedir);
			status = 1;
			break;
		}

		// A pipe to the NEXT stage — the last stage doesn't need one.
		int p[2] = { -1, -1 };
		if (i < nstages - 1 && os64_pipe(p) < 0)
		{
			os64_puts("husk: out of pipes\n");
			if (inRedir >= 0)  os64_close(inRedir);
			if (outRedir >= 0) os64_close(outRedir);
			status = 1;
			break;
		}

		// Slot priority: an explicit redirect beats the pipeline's plumbing.
		int in  = (inRedir  >= 0) ? inRedir  : prev_read;    // -1: console
		int out = (outRedir >= 0) ? outRedir
		        : (i < nstages - 1) ? p[1] : -1;             // -1: console

		// PATH resolution happens HERE, at spawn time — argv[0] stays the
		// name as typed (a program is told what it was called, not where it
		// was found; that's how busybox-style tricks stay possible someday).
		char pathbuf[256];
		const char *prog = resolve_command(cargv[0], pathbuf, sizeof(pathbuf));

		// The BACKGROUND flag rides all the way to task_create, because the
		// kernel has to know before the child's first instruction: a background
		// job's read of handle 0 returns EOF instead of competing with husk for
		// the keyboard. Output is untouched — it still prints to the screen.
		long pid = os64_spawn_redirected(prog, cargv, in, out, -1,
		                                 background ? OS64_SPAWN_BACKGROUND : 0);

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
			status = 1;
			break;
		}

		pids[npids++] = pid;
		prev_read = p[0];       // this stage's output becomes the next one's input
	}

	if (prev_read >= 0)
		os64_close(prev_read);  // belt and braces: never leave an end dangling

	// A BACKGROUND job is the one case where husk does not wait: it hands the
	// pipeline to the job table, prints "[1] 57", and goes straight back to the
	// prompt. The corpses are collected later by jobs_poll(). $? is 0 — the
	// LAUNCH succeeded; the job's own status arrives with "[1]+ 57 Done", which
	// is the only honest answer when the thing hasn't finished yet.
	if (background && npids > 0)
	{
		int jobNum = job_add(pids, npids);
		if (jobNum > 0)
			job_announce(jobNum, pids[npids - 1]);
		return status;
	}

	// Reap every child we launched. They run CONCURRENTLY — that is the point
	// of a pipeline; stage 2 is already chewing on stage 1's first bytes long
	// before stage 1 finishes. We just collect the corpses in order.
	int interrupted = 0;
	for (int i = 0; i < npids; i++)
	{
		int code = 0;
		long ended = os64_wait(pids[i], &code);
		report_exit(ended, code);
		if (code == 130)        // 128 + SIGINT: died by Ctrl+C
			interrupted = 1;
		status = code;          // reaped in launch order, so the last stage wins
	}

	// Echo the interrupt ONCE, after the whole pipeline is collected — the
	// victim was probably mid-line on the console, and this is the visible
	// answer to the keystroke (a real terminal echoes ^C at the keypress; we
	// echo at the funeral, ~10ms later, which reads the same to a human).
	if (interrupted)
		os64_write(1, "^C\n", 3);

	return status;
}

// ── the shell ───────────────────────────────────────────────────────────────

int main(int argc, char **argv, char **envp)
{
	(void)argc; (void)argv; (void)envp;

	os64_write(1, "husk — the os64 shell. `exit` to quit.\n",
	           sizeof("husk — the os64 shell. `exit` to quit.\n") - 1);
	os64_debug_log("husk: started");

	char line[LINE_MAX];
	char expanded[LINE_MAX + 512];  // headroom for expanded $CWD/$PATH values
	char *stages[MAX_STAGES];
	int last_status = 0;            // $? — nothing has failed yet

	for (;;)
	{
		// Bury any background job that finished while you were typing, BEFORE
		// the prompt is drawn — so the "[1]+ 57 Done" line lands above a fresh
		// prompt instead of halfway through the one you are looking at. This
		// is also the only reaping background jobs ever get, which is exactly
		// why they never pile up as zombies.
		jobs_poll();

		prompt();
		if (read_line(line, sizeof(line)) == 0)
			continue;

		// $? is substituted BEFORE the line is split or tokenized, so it
		// works anywhere on the line: `echo $?`, `cd $?` (weird, legal).
		expand_line(line, expanded, sizeof(expanded), last_status);

		// A trailing `&` applies to the WHOLE line, not the last stage:
		// `hello | upper &` backgrounds the pipeline, which is why this runs
		// before split_pipeline rather than inside it.
		int background = strip_background(expanded);
		if (expanded[0] == '\0')
			continue;           // the line was nothing but an `&`

		int nstages = split_pipeline(expanded, stages, MAX_STAGES);

		// `exit` is a builtin because it touches the SHELL'S OWN state (its
		// lifetime) — no separate program could ever do it. Checked WITHOUT
		// parse(), which tokenizes in place and would eat the line before
		// run_pipeline ever saw the arguments.
		if (nstages == 1 && first_token_is(stages[0], "exit"))
			break;

		// `export` is a builtin by the same physics as cd: the environment
		// copies DOWNWARD at spawn, never sideways or up, so an external
		// export would set its own copy and take it to the grave. Flat
		// model for now (no shell-variable tier — that arrives with husk
		// programmability): export KEY=VALUE goes straight to the env,
		// visible to every child spawned after. `export` alone lists the
		// environment, same walk env(1) does.
		if (nstages == 1 && first_token_is(stages[0], "export"))
		{
			char *eargv[ARGS_MAX];
			int eargc = parse(stages[0], eargv, ARGS_MAX);
			last_status = 0;
			if (eargc < 2)
			{
				// Bare `export`: list. The block is mapped right here in
				// our address space — walking it costs no syscalls.
				os64_envent_t e = { .index = 0 };
				while (os64_env_next(&e) == 0)
				{
					os64_puts(e.key);
					os64_puts("=");
					os64_puts(e.value);
					os64_puts("\n");
				}
			}
			else
			{
				// KEY=VALUE required; split at the FIRST '=' (values may
				// contain their own — TZ=EST5EDT,M3.2.0 has no '=' but a
				// PATH-like list someday might).
				char *eq = eargv[1];
				while (*eq && *eq != '=')
					eq++;
				if (*eq != '=' || eq == eargv[1])
				{
					os64_puts("husk: export: expected KEY=VALUE\n");
					last_status = 1;
				}
				else
				{
					*eq = 0;   // split in place; parse() already owns the line
					if (os64_setenv(eargv[1], eq + 1) != 0)
					{
						os64_puts("husk: export: failed (environment full?)\n");
						last_status = 1;
					}
				}
			}
			continue;
		}

		// `unset` — export's undo, builtin by the same one-way valve.
		// Unsetting the absent is success (idempotent since Bourne).
		if (nstages == 1 && first_token_is(stages[0], "unset"))
		{
			char *uargv[ARGS_MAX];
			int uargc = parse(stages[0], uargv, ARGS_MAX);
			if (uargc < 2)
			{
				os64_puts("husk: unset: expected a KEY\n");
				last_status = 1;
			}
			else
			{
				last_status = 0;
				for (int u = 1; u < uargc; u++)
					if (os64_unsetenv(uargv[u]) != 0)
						last_status = 1;
			}
			continue;
		}

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
				last_status = 1;    // builtins report through $? too
			}
			else
				last_status = 0;
			continue;
		}

		last_status = run_pipeline(stages, nstages, background);
	}

	os64_write(1, "husk: bye\n", 10);
	os64_debug_log("husk: exiting");
	return 0;
}
