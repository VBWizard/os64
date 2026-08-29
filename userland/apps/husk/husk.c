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
// v4: THE CONTROL LINE (2026-08-06, "the whole basket"). `>>` appends where
// `>` truncates (both are First Edition vocabulary, 1971 — and everyone
// truncates at least one file they loved learning the difference). `;`
// separates commands run in sequence; `&&` and `||` run the next command
// only on success / only on failure — nearly free because $? already
// existed, and they arrive in husk in the same order history added them
// (; was Thompson's, && and || came with Bourne). And `time` becomes a
// builtin PREFIX — see run_segment for why it can never be a utility.
// v5: STDERR (2026-08-28). `2> file`, `2>> file`, `2>&1`, `>&2`, `&> file`,
// `&>> file` — the third slot spawn had always offered, finally spelled. Same
// open-install-close discipline as slot 1; extract_redirections carries the
// vocabulary and the one deliberate departure from Bourne (`2>&1` binds to
// where stdout ENDS UP, not to where it was at that point on the line).
// v6: COMMAND SUBSTITUTION (2026-08-28). `$(cmd)` becomes what cmd printed —
// Korn's 1983 spelling, no backquote. The inner line runs in THIS shell one
// expansion depth down (expansion_ctx_t), its output captured through a pipe
// and marked as data like every other expansion. Tab completion arrived the
// same day, in the line editor.

#include "os64/os64.h"
#include "os64/conf.h"   // os64_conf_find — husk.rc rides the system search path now

#define LINE_MAX 256
#define ARGS_MAX 512        // raised from 16 for globbing; matches the kernel SPAWN_MAX_ARGS ceiling
#define MAX_STAGES 4          // a | b | c | d is plenty of rope for now
#define TASK_NAME_MAX 64      // lifecycle log identity; basename, always bounded

typedef struct {
	long tid;
	char name[TASK_NAME_MAX];
} launched_task_t;

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

// husk's own complaints go to handle 2, like every other program's. A shell
// that scolds you on stdout cannot be told `2> /dev/null` — and worse, its
// "cannot run" would be swallowed by a `> file` meant for the command's real
// output. The glob code already speaks to OS64_STDERR through os64_hprintf;
// these are the same door for the byte-at-a-time sites.
static void hputs(int h, const char *s)
{
	os64_write(h, s, os64_strlen(s));
}

static void err_puts(const char *s)
{
	hputs(OS64_STDERR, s);
}

static void err_num(unsigned long v)
{
	char b[20];
	int k = utoa(v, b);
	os64_write(OS64_STDERR, b, (unsigned)k);
}

// ── line editing ────────────────────────────────────────────────────────────
// (the prompt itself lives down beside the job table, which one of its
// escapes reports on; prompt_draw, beside the block assembler, is what
// main and the completion repaint both call — it knows whether the shell
// is prompting for a command or, inside an open `if`, for the rest of one)
static void prompt_draw(void);

// ── command history ─────────────────────────────────────────────────────────
// A ring of the last HISTORY_DEPTH submitted lines, recalled with Up/Down.
// The arrows arrive as VT100 escape sequences (ESC '[' A/B — the keyboard
// driver speaks 1979's vocabulary since 2026-08-04, and this parser is its
// first customer). A recalled line is edited exactly like a live one: the
// caret moves (Left/Right/Home/End, since 2026-08-08 and -16), and the first
// EDIT — not mere movement — makes the recalled line yours (browse resets).
//
// The classic contract, same as every shell since csh grew `!!` and ksh put
// arrows on it: Up walks backward through what you typed, Down walks
// forward, and walking past the newest entry returns you to the line you
// were composing when you started browsing (saved at first Up — losing the
// half-typed line to a stray arrow is the beginner trap this dodges).
// Duplicate suppression: a line identical to the previous entry is not
// stored twice (spamming `free` all night = ONE entry, Chris = the consumer).
#define HISTORY_DEPTH 32
static char s_history[HISTORY_DEPTH][LINE_MAX];
static int  s_hist_count = 0;    // entries stored (saturates at DEPTH)
static int  s_hist_next  = 0;    // ring slot the NEXT submit writes

static void history_store(const char *line)
{
	if (line[0] == '\0')
		return;                  // empty lines are not history
	if (s_hist_count > 0)
	{
		int last = (s_hist_next + HISTORY_DEPTH - 1) % HISTORY_DEPTH;
		if (os64_streq(s_history[last], line))
			return;              // same as the previous entry — once is enough
	}
	os64_strcopy(s_history[s_hist_next], LINE_MAX, line);
	s_hist_next = (s_hist_next + 1) % HISTORY_DEPTH;
	if (s_hist_count < HISTORY_DEPTH)
		s_hist_count++;
}

// Entry `back` steps into the past (1 = most recent). NULL when out of range.
static const char *history_get(int back)
{
	if (back < 1 || back > s_hist_count)
		return NULL;
	return s_history[(s_hist_next + HISTORY_DEPTH - back) % HISTORY_DEPTH];
}

// Swap the displayed line for `src`: rub out what's showing, echo the
// replacement, and update the buffer — the whole visual of history recall.
static void replace_line(char *buf, int *n, int cap, const char *src)
{
	while (*n > 0) { os64_write(1, "\b \b", 3); (*n)--; }
	int len = 0;
	while (src[len] != '\0' && len < cap - 1)
	{
		buf[len] = src[len];
		len++;
	}
	buf[len] = '\0';
	if (len > 0)
		os64_write(1, buf, (size_t)len);
	*n = len;
}

// Walk the visible cursor left by k cells. (The console's '\b' only MOVES —
// erasure stays overprint, per the renderer contract.)
static void caret_back(int k)
{
	static const char bs[8] = "\b\b\b\b\b\b\b\b";
	while (k > 0)
	{
		int chunk = k > 8 ? 8 : k;
		os64_write(1, bs, (size_t)chunk);
		k -= chunk;
	}
}

// Overprint k cells with blanks, advancing the cursor — caret_back's other
// half, and together they are the renderer's entire erase vocabulary.
static void blank_forward(int k)
{
	static const char sp[8] = "        ";
	while (k > 0)
	{
		int chunk = k > 8 ? 8 : k;
		os64_write(1, sp, (size_t)chunk);
		k -= chunk;
	}
}

// THE ONE DELETION ENGINE: remove buf[start, start+count) and leave the caret
// at the seam. Every erase gesture is this with different arithmetic —
// Backspace (start=pos-1, count=1), Delete (start=pos, count=1), Ctrl+U
// (start=0, count=pos), Ctrl+K (start=pos, count=n-pos), Ctrl+W (start=word,
// count=pos-word) — which is why the repaint lives here once instead of five
// times: walk the caret to the seam, shift the tail over the corpse, repaint
// the shifted tail, blank the orphaned cells, walk home.
static void edit_delete(char *buf, int *n, int *pos, int start, int count)
{
	caret_back(*pos - start);
	for (int i = start; i < *n - count; i++)
		buf[i] = buf[i + count];
	*n -= count;
	*pos = start;
	if (*pos < *n)
		os64_write(1, buf + *pos, (size_t)(*n - *pos));
	blank_forward(count);
	caret_back(*n - *pos + count);
}


// Insert `len` bytes AT the caret: shift the tail right, land the bytes,
// paint them plus the shifted tail, and walk the caret back to rest just
// after the insertion. At the end of the line this degenerates to the
// classic echo — one write, no backspaces. Bytes that would not fit are
// dropped, so the line never outgrows its buffer.
static void edit_insert(char *buf, int *n, int *pos, int cap,
                        const char *text, int len)
{
	if (len > cap - 1 - *n)
		len = cap - 1 - *n;
	if (len <= 0)
		return;
	for (int i = *n - 1; i >= *pos; i--)
		buf[i + len] = buf[i];
	for (int i = 0; i < len; i++)
		buf[*pos + i] = text[i];
	*n += len;
	os64_write(1, buf + *pos, (size_t)(*n - *pos));
	*pos += len;
	caret_back(*n - *pos);
}

// ── TAB COMPLETION ──────────────────────────────────────────────────────────
// TENEX's EXEC completed file names on ESC in 1972 — older than every other
// convenience in this line editor by a decade; tcsh moved it to Tab (1981)
// and bash followed (1989). The shape is the classic one: the word before
// the caret is completed against the directory it names — or, for the first
// word of a line with no '/' in it, against the builtins and then the
// current directory and PATH, where os64_resolve_command looks. One match
// is inserted whole (plus '/' for a directory, a space for anything else);
// several are extended to their longest common prefix; a Tab that can extend
// nothing lists them under the line and repaints it.
//
// No dotfile exception, same reason as the globs: os64 has no hidden-file
// convention to protect. The word runs back to the previous blank and quotes
// are not understood — this is the one place in husk that reads the line
// RAW — so a name with a space in it cannot be completed. A literal Tab can
// no longer be typed into a command; nothing in husk's vocabulary needs one.
#define COMP_MAX       256   // candidates kept — past this it is a listing nobody reads
#define COMP_NAME_MAX  64    // longer names are skipped, never truncated
#define COMP_LIST_COLS 100   // the listing's width budget, in cells (ls uses 5 x 20)

static char s_comp[COMP_MAX][COMP_NAME_MAX];
static bool s_comp_isdir[COMP_MAX];
static int  s_comp_count;

static int glob_strcmp(const char *a, const char *b);   // defined with the globs below

static bool str_starts(const char *s, const char *prefix)
{
	while (*prefix != '\0')
		if (*s++ != *prefix++)
			return false;
	return true;
}

// Keep `name` as a candidate if it starts with `prefix` and is not already
// kept (PATH directories can repeat, and a builtin can share a name with a
// program). Silently full at COMP_MAX.
static void comp_add(const char *name, const char *prefix, bool isdir)
{
	if (s_comp_count >= COMP_MAX || !str_starts(name, prefix))
		return;
	if (os64_strlen(name) >= COMP_NAME_MAX)
		return;
	for (int i = 0; i < s_comp_count; i++)
		if (str_eq(s_comp[i], name))
			return;
	os64_strcopy(s_comp[s_comp_count], COMP_NAME_MAX, name);
	s_comp_isdir[s_comp_count] = isdir;
	s_comp_count++;
}

// Every entry of `dir` starting with `prefix`. `runnable` skips directories:
// a command completion offers only what a spawn could take.
static void comp_scan_dir(const char *dir, const char *prefix, bool runnable)
{
	int64_t d = os64_opendir(dir);
	if (d < 0)
		return;
	os64_dirent_t e;
	while (s_comp_count < COMP_MAX && os64_readdir((int32_t)d, &e) == 1)
	{
		bool isdir = (e.flags & OS64_DE_DIR) != 0;
		if (runnable && isdir)
			continue;
		comp_add(e.name, prefix, isdir);
	}
	os64_close((int32_t)d);
}

// The builtins, the current directory, then each PATH directory — the order
// os64_resolve_command searches, though for completion order is only a courtesy.
static void comp_scan_commands(const char *prefix)
{
	static const char *const builtins[] = {
		"cd", "dirs", "exit", "export", "popd", "pushd", "time", "unset",
		// and the keywords, so `whi<Tab>` finishes a line the way a name does
		"if", "then", "elif", "else", "fi", "while", "for", "do", "done"
	};
	for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++)
		comp_add(builtins[i], prefix, false);

	char cwd[256];
	if (os64_getcwd(cwd, sizeof(cwd)) >= 0)
		comp_scan_dir(cwd, prefix, true);

	const char *path = os64_getenv("PATH");
	while (path != NULL && *path != '\0')
	{
		char dir[256];
		int n = 0;
		while (*path != '\0' && *path != ':' && n < (int)sizeof(dir) - 1)
			dir[n++] = *path++;
		if (*path == ':')
			path++;
		dir[n] = '\0';
		if (n > 0)
			comp_scan_dir(dir, prefix, true);
	}
}

static void comp_sort(void)
{
	for (int i = 1; i < s_comp_count; i++)
	{
		char key[COMP_NAME_MAX];
		bool keyDir = s_comp_isdir[i];
		os64_strcopy(key, sizeof(key), s_comp[i]);
		int j = i - 1;
		while (j >= 0 && glob_strcmp(s_comp[j], key) > 0)
		{
			os64_strcopy(s_comp[j + 1], COMP_NAME_MAX, s_comp[j]);
			s_comp_isdir[j + 1] = s_comp_isdir[j];
			j--;
		}
		os64_strcopy(s_comp[j + 1], COMP_NAME_MAX, key);
		s_comp_isdir[j + 1] = keyDir;
	}
}

// Print the candidates in columns under the line, then repaint the prompt
// and the line with the caret where it was.
static void comp_list_and_repaint(const char *buf, int n, int pos)
{
	int widest = 0;
	for (int i = 0; i < s_comp_count; i++)
	{
		int w = (int)os64_strlen(s_comp[i]) + (s_comp_isdir[i] ? 1 : 0);
		if (w > widest)
			widest = w;
	}
	int width = widest + 2;
	int cols = COMP_LIST_COLS / width;
	if (cols < 1)
		cols = 1;

	os64_write(1, "\n", 1);
	for (int i = 0; i < s_comp_count; i++)
	{
		int w = (int)os64_strlen(s_comp[i]);
		os64_write(1, s_comp[i], (size_t)w);
		if (s_comp_isdir[i])
		{
			os64_write(1, "/", 1);
			w++;
		}
		bool rowEnd = ((i + 1) % cols == 0) || (i + 1 == s_comp_count);
		if (rowEnd)
			os64_write(1, "\n", 1);
		else
			blank_forward(width - w);
	}
	prompt_draw();
	if (n > 0)
		os64_write(1, buf, (size_t)n);
	caret_back(n - pos);
}

// Complete the word that ends at the caret, editing the line in place.
static void complete_at_caret(char *buf, int *n, int *pos, int cap)
{
	int ws = *pos;
	while (ws > 0 && buf[ws - 1] != ' ' && buf[ws - 1] != '\t')
		ws--;
	int wlen = *pos - ws;
	char word[LINE_MAX];
	for (int i = 0; i < wlen; i++)
		word[i] = buf[ws + i];
	word[wlen] = '\0';

	bool firstWord = true;
	for (int i = 0; i < ws; i++)
		if (buf[i] != ' ' && buf[i] != '\t')
			firstWord = false;
	const char *lastSlash = NULL;
	for (const char *p = word; *p != '\0'; p++)
		if (*p == '/')
			lastSlash = p;

	s_comp_count = 0;
	const char *prefix;
	if (firstWord && lastSlash == NULL)
	{
		// A command name. An EMPTY first word is left alone: a stray Tab at
		// a bare prompt would otherwise dump every program on the system.
		if (wlen == 0)
			return;
		prefix = word;
		comp_scan_commands(prefix);
	}
	else if (lastSlash != NULL)
	{
		// A path: the directory is everything through the last '/', and
		// the prefix is what follows it.
		char dir[LINE_MAX];
		int dlen = (int)(lastSlash - word) + 1;
		for (int i = 0; i < dlen; i++)
			dir[i] = word[i];
		dir[dlen] = '\0';
		prefix = lastSlash + 1;
		comp_scan_dir(dir, prefix, false);
	}
	else
	{
		// A bare name after the command: an entry of the current directory.
		char cwd[256];
		if (os64_getcwd(cwd, sizeof(cwd)) < 0)
			return;
		prefix = word;
		comp_scan_dir(cwd, prefix, false);
	}
	if (s_comp_count == 0)
		return;                          // nothing matches: the line is yours as typed
	comp_sort();

	int plen = (int)os64_strlen(prefix);
	if (s_comp_count == 1)
	{
		// One answer: the rest of the name, then the thing that follows a
		// finished name — a '/' to keep walking into a directory, a space
		// to move on to the next argument.
		edit_insert(buf, n, pos, cap, s_comp[0] + plen, (int)os64_strlen(s_comp[0]) - plen);
		edit_insert(buf, n, pos, cap, s_comp_isdir[0] ? "/" : " ", 1);
		return;
	}

	// Several: extend to their longest common prefix if that says anything
	// new, otherwise show them.
	int lcp = (int)os64_strlen(s_comp[0]);
	for (int i = 1; i < s_comp_count; i++)
	{
		int k = 0;
		while (k < lcp && s_comp[i][k] == s_comp[0][k])
			k++;
		lcp = k;
	}
	if (lcp > plen)
		edit_insert(buf, n, pos, cap, s_comp[0] + plen, lcp - plen);
	else
		comp_list_and_repaint(buf, *n, *pos);
}
// ── typeahead pushback ──────────────────────────────────────────────────────
// A `while` loop polls the console between iterations for a Ctrl+C
// (loop_interrupted). Any other byte it finds was typed ahead and belongs to
// the next prompt, so it is parked here and read_line drains it first —
// a running loop must not eat your typing. Bounded; past the bound a byte
// is dropped, which is what a full tty queue does too.
#define PUSHBACK_MAX 64
static char s_pushback[PUSHBACK_MAX];
static int  s_pushback_n = 0;      // bytes parked
static int  s_pushback_i = 0;      // next to hand out

static void pushback_put(char c)
{
	if (s_pushback_n < PUSHBACK_MAX)
		s_pushback[s_pushback_n++] = c;
}

// One byte for the line editor: the pushback first, then the console.
static int64_t console_byte(char *c)
{
	if (s_pushback_i < s_pushback_n)
	{
		*c = s_pushback[s_pushback_i++];
		if (s_pushback_i == s_pushback_n)
			s_pushback_i = s_pushback_n = 0;
		return 1;
	}
	return os64_read(0, c, 1);
}

// Read one line from the console into buf (NUL-terminated), echoing as we go.
// Returns the length, or -1 for a Ctrl+C at the prompt. Handles Enter
// (submit), Backspace (erase before the
// caret), Left/Right caret movement with mid-line insert (2026-08-08 — the
// day the console grew a real cursor to make it visible), Up/Down history
// recall, and — since 2026-08-16 — Delete (erase AT the caret), Home/End,
// and the control chords every terminal has answered to since the ASR-33
// era gave way to CRTs: Ctrl+A/E (home/end, emacs's spelling), Ctrl+U (kill
// to start — V7's line-kill, promoted from @), Ctrl+K (kill to end), and
// Ctrl+W (word erase — 4BSD's werase, the one Bill Joy typed). Tab completes
// the word at the caret (complete_at_caret, above).
//
// KNOWN LIMIT, shared with history recall since birth: the renderer's '\b'
// clamps at column 0, so editing a line that has WRAPPED misbehaves at the
// wrap seam. A line that long deserves a script file anyway.
static int read_line(char *buf, int cap)
{
	int n = 0;
	int pos = 0;                 // the caret: insertion point, 0..n
	int browse = 0;              // 0 = composing live; k = viewing history[k back]
	char live[LINE_MAX];         // the half-typed line, parked during browsing
	live[0] = '\0';

	for (;;)
	{
		char c;
		if (console_byte(&c) != 1)
			continue;

		if (c == '\r' || c == '\n')
		{
			// Submit takes the WHOLE line no matter where the caret sits —
			// it is already fully painted on screen.
			os64_write(1, "\n", 1);
			buf[n] = 0;
			history_store(buf);
			return n;
		}
		if (c == 0x08 || c == 0x7f)          // backspace: delete BEFORE the caret
		{
			if (pos > 0)
				edit_delete(buf, &n, &pos, pos - 1, 1);
			browse = 0;          // editing makes the recalled line YOURS now
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
			// -1, not 0: an empty line inside an open `if` is nothing, a
			// Ctrl+C there abandons the whole block.
			os64_write(1, "^C\n", 3);
			buf[0] = 0;
			return -1;
		}
		if (c == 0x1B)                       // ESC — a VT100 sequence begins
		{
			// The keyboard delivers arrows as ESC '[' <final> in one burst;
			// the two follow-up reads block only in the pathological case of
			// a bare ESC from some future source, which no key produces today.
			char seq[2];
			if (console_byte(&seq[0]) != 1 || seq[0] != '[')
				continue;        // lone ESC or unknown: swallow
			if (console_byte(&seq[1]) != 1)
				continue;

			// The digit-parameter family: ESC [ <n> ~ (Delete=3, Insert=2,
			// PgUp=5, PgDn=6 — xterm's vocabulary for the keys the VT100
			// lacked). The trailing '~' must be CONSUMED even for sequences
			// we ignore: before 2026-08-16 this parser swallowed the digit
			// and let the '~' fall through as a printable, so PgUp at the
			// prompt quietly typed a tilde into the command.
			char param = 0;
			if (seq[1] >= '0' && seq[1] <= '9')
			{
				param = seq[1];
				char tilde;
				if (console_byte(&tilde) != 1 || tilde != '~')
					continue;    // malformed burst: swallow what we saw
				seq[1] = '~';
			}

			if (seq[1] == 'A')               // Up — one step further back
			{
				if (history_get(browse + 1) == NULL)
					continue;    // no further past; nothing visibly changes
				// Browsing swaps the WHOLE line, and replace_line rubs out
				// from the caret — so the caret goes to the end first
				// (advancing over glyphs already on screen costs a re-echo).
				if (pos < n)
				{
					os64_write(1, buf + pos, (size_t)(n - pos));
					pos = n;
				}
				if (browse == 0)
				{
					buf[n] = '\0';           // park the half-typed line
					os64_strcopy(live, LINE_MAX, buf);
				}
				browse++;
				replace_line(buf, &n, cap, history_get(browse));
				pos = n;
			}
			else if (seq[1] == 'B')          // Down — one step toward now
			{
				if (browse == 0)
					continue;    // already composing; Down has nowhere to go
				if (pos < n)
				{
					os64_write(1, buf + pos, (size_t)(n - pos));
					pos = n;
				}
				browse--;
				replace_line(buf, &n, cap,
				             browse == 0 ? live : history_get(browse));
				pos = n;
			}
			else if (seq[1] == 'D')          // Left — caret one cell back
			{
				if (pos > 0)
				{
					pos--;
					os64_write(1, "\b", 1);
				}
				// Pure movement is not editing: browse survives, so an Up
				// after a stray Left still walks history from where it was.
			}
			else if (seq[1] == 'C')          // Right — caret one cell forward
			{
				if (pos < n)
				{
					// Re-echo the glyph under the caret: same pixels, and
					// the console cursor advances one cell — movement by
					// overprint, the only vocabulary the renderer needs.
					os64_write(1, buf + pos, 1);
					pos++;
				}
			}
			else if (seq[1] == 'H')          // Home — caret to column one
			{
				caret_back(pos);
				pos = 0;
				// Movement, not editing: browse survives, same as Left/Right.
			}
			else if (seq[1] == 'F')          // End — caret past the last glyph
			{
				if (pos < n)
				{
					os64_write(1, buf + pos, (size_t)(n - pos));
					pos = n;
				}
			}
			else if (seq[1] == '~' && param == '3')   // Delete — erase AT the caret
			{
				if (pos < n)
					edit_delete(buf, &n, &pos, pos, 1);
				browse = 0;      // editing makes the recalled line YOURS now
			}
			// Other '~' sequences (Insert, PgUp, PgDn) have no line-editing
			// meaning: swallowed whole, tilde and all.
			continue;
		}
		// The line-editing control chords (Ctrl+letter arrives as 0x01..0x1A
		// — the 1963 design working as designed). The kill chords are older
		// than the arrow keys they now live beside: V7's tty driver already
		// had a line-kill character, and 4BSD added word-erase.
		if (c == 0x01)                       // Ctrl+A — home (emacs's spelling)
		{
			caret_back(pos);
			pos = 0;
			continue;                        // movement: browse survives
		}
		if (c == 0x05)                       // Ctrl+E — end
		{
			if (pos < n)
			{
				os64_write(1, buf + pos, (size_t)(n - pos));
				pos = n;
			}
			continue;
		}
		if (c == 0x15)                       // Ctrl+U — kill to start of line
		{
			if (pos > 0)
				edit_delete(buf, &n, &pos, 0, pos);
			browse = 0;
			continue;
		}
		if (c == 0x0B)                       // Ctrl+K — kill to end of line
		{
			if (pos < n)
				edit_delete(buf, &n, &pos, pos, n - pos);
			browse = 0;
			continue;
		}
		if (c == 0x17)                       // Ctrl+W — erase the word before the caret
		{
			// The classic gait: step over any spaces behind the caret, then
			// over the word itself — so a caret resting after "ls  " kills
			// "ls  ", not nothing.
			int j = pos;
			while (j > 0 && buf[j - 1] == ' ')
				j--;
			while (j > 0 && buf[j - 1] != ' ')
				j--;
			if (j < pos)
				edit_delete(buf, &n, &pos, j, pos - j);
			browse = 0;
			continue;
		}
		if (c == '\t')                       // Tab — complete the word at the caret
		{
			complete_at_caret(buf, &n, &pos, cap);
			browse = 0;          // a completion is an edit: the line is YOURS now
			continue;
		}
		// Any other control chord has no line-editing meaning yet — swallow
		// it rather than burying invisible bytes in the command.
		if ((unsigned char)c < 0x20)
			continue;
		edit_insert(buf, &n, &pos, cap, &c, 1);
		browse = 0;              // typing makes the recalled line YOURS now
	}
}

// ── WILDCARDS (2026-08-13) ──────────────────────────────────────────────────
//
// THE SHELL EXPANDS THEM, NOT THE PROGRAMS. This is the 1971 decision, and it
// is the whole reason `cat /tmp/*` can work at all: CP/M and MS-DOS made every
// program expand its own wildcards, so each one reimplemented the rules and
// they disagreed with each other — COPY understood *.TXT, some other tool
// didn't. Unix put it in the shell exactly once. Consequence here: cat, head,
// ls, grep and wc needed ZERO changes to gain wildcard support. They simply
// receive more argv entries and never learn that '*' exists.
//
// Expansion happens inside parse() on purpose, because parse() is the only
// place that still knows which characters were QUOTED. It strips quotes as it
// goes, so by the time anyone holds argv[] a '*' from `"*"` is indistinguishable
// from a bare one — and `echo "*"` must print an asterisk, not the directory.
//
// v1 globs the LAST path component only: /tmp/* yes, /*/foo no (booked, not
// hidden). No special case for leading dots either — that rule exists because
// Unix hides dotfiles, and os64 has no hidden-file convention to protect
// (husk.rc, not .huskrc). Inheriting the exception without the reason would be
// exactly the jargon this project declines.
#define GLOB_POOL_BYTES  (512 * 256)   // the 512x256 ceiling, in BSS: demand-paged, so untouched pages cost nothing
#define GLOB_PATH_MAX    256

static char   s_glob_pool[GLOB_POOL_BYTES];
static size_t s_glob_used;

// Byte-order compare, for sorting matches. libos64 offers os64_streq, which
// answers equality but not order — kept local here alongside husk's other
// freestanding helpers rather than growing the library for one caller.
static int glob_strcmp(const char *a, const char *b)
{
	while (*a != '\0' && *a == *b) { a++; b++; }
	return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

// A bracket starts a pattern class only when the class has an end. The first
// ']' is a literal member, including after a leading negation marker.
static bool glob_class_closes(const char *open)
{
	const char *p = open + 1;
	if (*p == '!' || *p == '^')
		p++;
	if (*p == ']')
		p++;
	for (; *p != '\0'; p++)
		if (*p == ']')
			return true;
	return false;
}

static bool glob_has_meta(const char *s)
{
	for (; *s; s++)
		if (*s == '*' || *s == '?' ||
		    (*s == '[' && glob_class_closes(s)))
			return true;
	return false;
}

// V7 pattern match: '*' any run, '?' one character, [abc] [a-z] [!a-z] sets.
// An '[' with no closing ']' is an ordinary character, as it is in the shell
// input that invokes /bin/[.
// Recursive on '*' — the classic formulation, and the recursion depth is
// bounded by the number of '*' in the pattern, not by the name length.
static bool glob_match(const char *p, const char *n)
{
	while (*p)
	{
		if (*p == '*')
		{
			p++;
			if (*p == '\0')
				return true;                  // trailing '*' takes the rest
			for (const char *t = n; ; t++)
			{
				if (glob_match(p, t))
					return true;
				if (*t == '\0')
					return false;
			}
		}
		if (*n == '\0')
			return false;
		if (*p == '?')
		{
			p++; n++;
			continue;
		}
		if (*p == '[')
		{
			const char *q = p + 1;
			bool negate = (*q == '!' || *q == '^');
			if (negate)
				q++;
			bool hit = false;
			bool first = true;
			// `first` lets a ']' immediately after '[' (or after '!') be a
			// literal member, the way every shell since the Bourne shell does.
			while (*q != '\0' && (*q != ']' || first))
			{
				first = false;
				if (q[1] == '-' && q[2] != '\0' && q[2] != ']')
				{
					if ((unsigned char)*n >= (unsigned char)q[0] &&
					    (unsigned char)*n <= (unsigned char)q[2])
						hit = true;
					q += 3;
				}
				else
				{
					if (*n == *q)
						hit = true;
					q++;
				}
			}
			if (*q != ']')
			{
				if (*p != *n)
					return false;
				p++;
				n++;
				continue;
			}
			if (hit == negate)
				return false;
			p = q + 1;
			n++;
			continue;
		}
		if (*p != *n)
			return false;
		p++; n++;
	}
	return *n == '\0';
}

// Copy `s` into the expansion pool and return the stored pointer, or NULL if
// the pool is full. The pool cannot be the line buffer: parse() tokenizes in
// place, and one token can become many arguments.
static char *glob_pool_add(const char *dirPrefix, const char *name)
{
	size_t plen = dirPrefix ? os64_strlen(dirPrefix) : 0;
	size_t nlen = os64_strlen(name);
	if (s_glob_used + plen + nlen + 1 > sizeof(s_glob_pool))
		return NULL;

	char *out = s_glob_pool + s_glob_used;
	for (size_t i = 0; i < plen; i++)
		out[i] = dirPrefix[i];
	for (size_t i = 0; i <= nlen; i++)       // <= copies the NUL
		out[plen + i] = name[i];
	s_glob_used += plen + nlen + 1;
	return out;
}

// Expand one metacharacter-bearing token into argv[] starting at `argc`,
// returning the new argc, or -1 on failure (message already printed).
// Matches are inserted in sorted order — Unix has guaranteed sorted glob
// results since the beginning, which is why `*` is reproducible.
static int glob_expand(const char *token, char *argv[], int argc, int maxargs)
{
	// Split into the directory to read and the pattern to match. Only the last
	// component may contain metacharacters (see the v1 note above).
	char dirPath[GLOB_PATH_MAX];
	char dirPrefix[GLOB_PATH_MAX];           // exactly as typed, re-attached to each match
	const char *pattern = token;
	const char *slash = 0;
	for (const char *s = token; *s; s++)
		if (*s == '/')
			slash = s;

	if (slash != 0)
	{
		size_t plen = (size_t)(slash - token) + 1;   // include the '/'
		if (plen >= sizeof(dirPrefix))
		{
			os64_hprintf(OS64_STDERR, "husk: pattern too long: %s\n", token);
			return -1;
		}
		for (size_t i = 0; i < plen; i++)
			dirPrefix[i] = token[i];
		dirPrefix[plen] = '\0';
		// "/x" -> dir "/", otherwise drop the trailing slash for opendir.
		os64_strcopy(dirPath, sizeof(dirPath), dirPrefix);
		if (plen > 1)
			dirPath[plen - 1] = '\0';
		pattern = slash + 1;
	}
	else
	{
		// No directory in the token: read the current directory, and attach no
		// prefix so `cat *` yields bare names exactly as typed.
		dirPrefix[0] = '\0';
		if (os64_getcwd(dirPath, sizeof(dirPath)) < 0)
		{
			os64_hprintf(OS64_STDERR, "husk: cannot read the current directory\n");
			return -1;
		}
	}

	// A trailing-slash-only pattern ("/tmp/") has nothing to match.
	if (*pattern == '\0')
	{
		os64_hprintf(OS64_STDERR, "husk: no match: %s\n", token);
		return -1;
	}

	int64_t d = os64_opendir(dirPath);
	if (d < 0)
	{
		os64_hprintf(OS64_STDERR, "husk: cannot read directory %s\n", dirPath);
		return -1;
	}

	int first = argc;                        // matches from THIS token sort among themselves
	os64_dirent_t e;
	while (os64_readdir((int32_t)d, &e) == 1)
	{
		if (!glob_match(pattern, e.name))
			continue;

		if (argc >= maxargs - 1)
		{
			os64_close((int32_t)d);
			os64_hprintf(OS64_STDERR,
				"husk: %s expands past the %d-argument limit\n", token, maxargs - 1);
			return -1;
		}

		char *stored = glob_pool_add(dirPrefix, e.name);
		if (stored == 0)
		{
			os64_close((int32_t)d);
			os64_hprintf(OS64_STDERR, "husk: too much expanded text for %s\n", token);
			return -1;
		}

		// Insertion sort into this token's own run.
		int at = argc;
		while (at > first && glob_strcmp(argv[at - 1], stored) > 0)
		{
			argv[at] = argv[at - 1];
			at--;
		}
		argv[at] = stored;
		argc++;
	}
	os64_close((int32_t)d);

	if (argc == first)
	{
		// CHRIS'S RULING (2026-08-13): a pattern that matches nothing REFUSES
		// the command rather than being passed through literally. csh/zsh/fish
		// behavior, not Bourne's — and the honest one here: handing `cat` a
		// filename of "*.xyz" produces a confusing complaint about a file
		// nobody meant, where this says exactly what went wrong. It is also
		// the house tripwire doctrine: fail loudly at the boundary.
		os64_hprintf(OS64_STDERR, "husk: no match: %s\n", token);
		return -1;
	}
	return argc;
}

// Tokenize `line` in place into argv[], removing matching single or double
// quotes. Quotes group spaces into one argument; the child receives only the
// resulting bytes and never needs to know shell syntax existed.
//
// Returns argc, or -1 if a wildcard matched nothing (the message is printed by
// glob_expand; callers must treat <0 as "already reported, do not run").
// ── what came out of an expansion is DATA, not syntax ───────────────────────
//
// husk expands a line into a flat string and then parses that string, which
// is the shell's oldest shape and its oldest trap: the bytes a value CONTAINS
// get read back as operators. A script line as ordinary as
//
//     echo $1
//
// would open a file when $1 held "> /etc/husk.rc", or start a pipeline when it
// held "| rm x" — the argument reclassified as syntax by the very act of
// substituting it. No shell has ever worked that way: the Bourne shell splits
// expansion results into WORDS (and globs them), but it does not go back and
// look for `>` or `|` in them, precisely because a program's arguments arrive
// from outside and must not be able to rewrite the command that received them.
// The positional parameters made this reachable from outside husk for the
// first time — a script's arguments are whatever the caller typed. (Codex
// review, 2026-08-22.)
//
// The cure is one bit per byte. expand_line fills a mask parallel to the
// expanded line, marking every byte it SUBSTITUTED as against every byte the
// author TYPED, and the three places that recognize an operator ask the mask
// first. Nothing else changes: word splitting still happens (`echo $*` is
// still several arguments), globbing still happens (`x=*.c; echo $x` still
// matches files, as it does in every shell), and quoting inside a value is
// still processed by parse() — a divergence from POSIX that predates this and
// is left alone deliberately, since it is the harmless direction: it can only
// group or split words, never redirect output or spawn a pipeline.
#define EXPANDED_MAX (LINE_MAX + 512)

// ── the expansion context, one per substitution depth ───────────────────────
// A `$(...)` runs a whole command line WHILE the line containing it is being
// expanded, so the expanded text, its mask and the captured output cannot be
// one static set — the inner line would overwrite the outer's buffer in the
// middle of filling it. One context per depth; s_expdepth names the one in
// use, 0 for a line that arrived from the keyboard, the rc or a script.
// STATIC rather than stack frames, because s_expbase/s_expmask describe the
// buffer for as long as parse() reads it.
#define SUBST_DEPTH_MAX 3
typedef struct {
	char    text[EXPANDED_MAX];      // the expanded line run_expanded parses
	uint8_t mask[EXPANDED_MAX];      // 1 = this byte came from an expansion
	char    capture[EXPANDED_MAX];   // what the line's commands wrote to stdout
	int     captureLen;
	bool    captureOverflow;         // more arrived than fits: the substitution is refused
	bool    substRan;                // a $(...) ran while THIS depth's line was expanding
	int     substStatus;             // ...and this is what it answered (the last one, if several)
} expansion_ctx_t;
static expansion_ctx_t s_expctx[SUBST_DEPTH_MAX + 1];
static int s_expdepth = 0;

static uint8_t *s_expmask = NULL;         // the mask of the buffer parse() is reading
static const char *s_expbase = NULL;      // that buffer
static int s_explen = 0;                  // how much of it is live

// Set by expand_line when it has already explained a refusal, so run_segment
// does not add a second, wrong explanation on top.
static bool s_expand_complained = false;

static int run_line(char *line, int *last_status);   // a substitution runs one

// Given the text just after a `$(`, find its closing `)`: parens nest, and
// quotes hide parens (`$(echo ")")` is one substitution). NULL when the
// text ends first.
static const char *find_subst_close(const char *p)
{
	int depth = 1;
	char quote = 0;
	for (; *p != '\0'; p++)
	{
		if (quote != 0)
		{
			if (*p == quote)
				quote = 0;
		}
		else if (*p == '\'' || *p == '"')
			quote = *p;
		else if (*p == '(')
			depth++;
		else if (*p == ')' && --depth == 0)
			return p;
	}
	return NULL;
}

// Did this byte come out of an expansion? Anything outside the expanded
// buffer (a glob-pool string, a literal) counts as typed — the answer only
// ever has to be right for bytes the parser is scanning.
static bool byte_is_expanded(const char *p)
{
	if (s_expbase == NULL || p == NULL || p < s_expbase || p >= s_expbase + s_explen)
		return false;
	return s_expmask[p - s_expbase] != 0;
}

// Mark a byte as DATA outright. parse() uses this for bytes it copies out of
// a quoted region: the quotes are removed during tokenization, and without
// this the fact of having been quoted is removed with them.
static void byte_mask_mark(const char *p)
{
	if (s_expbase == NULL || p < s_expbase || p >= s_expbase + s_explen)
		return;
	s_expmask[p - s_expbase] = 1;
}

// Is this token entirely the author's own typing? Only such a token may be an
// operator. Two sources of NOT-typed bytes exist, and both are the outside
// world: a $-expansion (the mask above) and a GLOB result, which is a
// filename — the filesystem's text, not the shell's. Glob results live in
// their own pool rather than in the expanded line, so they are recognized by
// address rather than by mask.
static bool token_is_typed(const char *t)
{
	if (t == NULL)
		return false;
	if (t >= s_glob_pool && t < s_glob_pool + sizeof(s_glob_pool))
		return false;                       // a filename the glob found
	for (const char *p = t; *p != '\0'; p++)
		if (byte_is_expanded(p))
			return false;                   // any substituted byte disqualifies it
	return true;
}

// parse() compacts tokens leftward IN PLACE; the mask has to travel with the
// bytes or it would describe the pre-compaction line. Destination is always
// at or left of the source, and the scan is left to right, so a moved flag
// can never clobber one still waiting to be read.
static void byte_mask_move(const char *to, const char *from)
{
	if (s_expbase == NULL)
		return;
	if (to < s_expbase || to >= s_expbase + s_explen)
		return;
	if (from < s_expbase || from >= s_expbase + s_explen)
		return;
	s_expmask[to - s_expbase] = s_expmask[from - s_expbase];
}

static int parse(char *line, char *argv[], int maxargs)
{
	int argc = 0;
	char *readp = line;
	char *writep = line;

	// One command line's worth of expansions. Safe to reset per call: every
	// caller parses a stage, spawns it (which copies the strings into the
	// kernel), and only then parses the next.
	s_glob_used = 0;

	while (*readp && argc < maxargs - 1)
	{
		while (*readp == ' ' || *readp == '\t') readp++;
		if (!*readp) break;

		char *token = writep;
		// Tracked HERE and nowhere else: this is the only point in husk that
		// still knows a metacharacter arrived unquoted. `echo "*"` must print
		// a star.
		bool unquotedMeta = false;
		char quote = 0;
		while (*readp)
		{
			// THE QUOTE STATE MACHINE ANSWERS ONLY TO TYPED QUOTES, in both
			// directions. A substituted quote byte is inert here: it neither
			// opens a region nor closes one, and falls through to the copy
			// below as the ordinary character it is.
			//
			// This settles the divergence the first version of this fix left
			// standing (and flagged for a ruling): parse() used to process
			// quotes inside a value, so `$x` holding `"a b"` arrived as one
			// argument. That grouping cannot be kept, because it IS the
			// escape — an expanded quote that can close a region can end the
			// author's quoting early, and a `>` the author had safely quoted
			// becomes a token of its own and redirects. Real shells do not
			// re-scan expansion results for quotes for exactly this reason.
			// (Codex review, 2026-08-23.)
			if (quote != 0)
			{
				if (*readp == quote && !byte_is_expanded(readp))
				{
					quote = 0;
					readp++;
					continue;
				}
				// QUOTED BYTES ARE DATA, and this is the line that makes
				// quoting mean anything to a redirection. The quotes come
				// off here — that is what tokenizing does — and until this
				// mark existed, nothing downstream could tell `echo '>' hi`
				// from `echo > hi`, so a quoted operator redirected anyway
				// and printed nothing. (Pipes escaped the bug by accident:
				// split_pipeline runs BEFORE the quotes are stripped and so
				// still sees them. Found re-reading this file as its own
				// reviewer, 2026-08-23; it predates the expansion work.)
				byte_mask_mark(writep);
				*writep++ = *readp++;
				continue;
			}

			if ((*readp == '\'' || *readp == '"') && !byte_is_expanded(readp))
			{
				quote = *readp++;
				continue;
			}
			if (*readp == ' ' || *readp == '\t')
				break;
			if (*readp == '*' || *readp == '?' || *readp == '[')
				unquotedMeta = true;
			byte_mask_move(writep, readp);
			*writep++ = *readp++;
		}
		while (*readp == ' ' || *readp == '\t') readp++;
		*writep++ = 0;

		if (unquotedMeta && glob_has_meta(token))
		{
			int expanded = glob_expand(token, argv, argc, maxargs);
			if (expanded < 0)
				return -1;               // no match / overflow — already reported
			argc = expanded;
		}
		else
			argv[argc++] = token;
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

// Split a line on unquoted '|' characters into stage strings, in place.
// Quotes stay in the stage for parse() to remove later. Thus
// `grep -E "AP2|AP3"` is one grep, while `cat f | grep AP3` is a pipeline.
static int split_pipeline(char *line, char *stages[], int maxstages)
{
	int n = 0;
	char *p = line;
	char quote = 0;

	stages[n++] = p;
	while (*p && n < maxstages)
	{
		if (quote != 0)
		{
			// CLOSING is guarded exactly like opening, and the first cut of
			// this fix guarded only one of them — which left the hole intact
			// in mirror image. `echo "$1|touch /home/pwn"` with $1 holding a
			// single `"` let the SUBSTITUTED quote close the AUTHOR'S quote,
			// after which the `|` the author had safely quoted stood in the
			// open and split a pipeline. Data could not create syntax any
			// more, so it created it by DESTROYING the quoting around it.
			// (Codex review, 2026-08-23 — a second look at the first fix.)
			if (*p == quote && !byte_is_expanded(p))
				quote = 0;
			p++;
		}
		else if ((*p == '\'' || *p == '"') && !byte_is_expanded(p))
		{
			// A quote INSIDE a substituted value is data — otherwise a lone
			// apostrophe in $1 would open a quote here and swallow the real
			// `|` that follows it.
			quote = *p++;
		}
		else if (*p == '|' && !byte_is_expanded(p))
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

// Split a line on unquoted `;`, `&&`, and `||` into a COMMAND LIST, in
// place. seps[i] records what connects cmds[i] to cmds[i+1]. Single `|`
// (a pipe) and single `&` (background) are deliberately NOT separators —
// the two-character operators are consumed here precisely so
// split_pipeline never mistakes `||` for two pipes downstream.
//
// Splitting happens on the RAW line, BEFORE $-expansion, for a reason
// with teeth: `false ; echo $?` must print 1, which means each command
// expands at ITS OWN execution time, after its predecessor has updated
// the status — one pre-expanded pass over the whole line would hand
// every segment yesterday's $?.
//
// Returns the command count, or -1 when the line holds more than maxcmds
// commands. Refusing is the tripwire rule: the first draft just stopped
// scanning at the cap, which silently glued " ; ninth" onto command
// eight as LITERAL ARGV TOKENS — a bounded parser must say so out loud
// when the line outgrows it, never reinterpret the overflow as data.
#define MAX_CMDS 8
enum { SEP_SEQ, SEP_AND, SEP_OR };

static int split_commands(char *line, char *cmds[], int seps[], int maxcmds)
{
	int n = 0;
	char *p = line;
	char quote = 0;

	cmds[n++] = p;
	while (*p)
	{
		if (quote != 0)
		{
			if (*p == quote)
				quote = 0;
			p++;
		}
		else if (*p == '\'' || *p == '"')
		{
			quote = *p++;
		}
		else if (p[0] == '$' && p[1] == '(')
		{
			// A substitution's inner text belongs to the line that runs it:
			// its `;` and `&&` are THAT line's separators. Unterminated, the
			// rest of the line is swallowed here and expand_line refuses it.
			const char *close = find_subst_close(p + 2);
			p = (close != NULL) ? (char *)close + 1 : p + os64_strlen(p);
		}
		else if (*p == ';')
		{
			if (n >= maxcmds)
				return -1;
			*p++ = 0;
			seps[n - 1] = SEP_SEQ;
			cmds[n++] = p;
		}
		else if (p[0] == '&' && p[1] == '&')
		{
			if (n >= maxcmds)
				return -1;
			*p = 0;
			p += 2;
			seps[n - 1] = SEP_AND;
			cmds[n++] = p;
		}
		else if (p[0] == '|' && p[1] == '|')
		{
			if (n >= maxcmds)
				return -1;
			*p = 0;
			p += 2;
			seps[n - 1] = SEP_OR;
			cmds[n++] = p;
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
//   $$     the shell's own task ID (Bourne, 1977 — born so scripts could
//          mint unique temp-file names: /tmp/sort$$ and friends). Expanded
//          HERE, before any child exists, so `echo $$` prints HUSK's id no
//          matter who echo is — expansion-time identity, the counterpart to
//          /proc/self's open-time identity (that one names the opener,
//          which is why `cat /proc/self/status` reports on cat)
//   $CWD   the current directory, fetched LIVE from the kernel at expansion
//          time. Unix's $PWD (csh's $cwd, 1978) is a shell-maintained COPY
//          of kernel state, patched by hand on every cd and famous for
//          drifting; os64 declines the cache and asks the owner — the truth
//          costs one syscall and can never be stale.
//   $NAME  the env block (os64_getenv). An unset name expands to nothing —
//          Bourne's rule; a literal "$NOPE" in the output helps nobody.
//   $(cmd) what cmd printed, trailing newlines stripped — run_substitution
//          carries the rules and the lineage (Korn, 1983)
// A '$' that starts no name ($ alone, "$5", "$/") stays a literal '$'.
//
// QUOTING (2026-08-09, the day `watch` made it matter): SINGLE quotes suppress
// expansion, DOUBLE quotes do not. That is Bourne's 1977 split, and it is the
// entire reason a shell needs two quote characters instead of one — '' hands
// the bytes through untouched, "" groups words while still letting the shell
// speak. Before this, expansion walked the line quote-blind and '$CWD' meant
// exactly what "$CWD" meant, which is a difference nobody misses until a
// program takes a COMMAND LINE as an argument: `watch 'echo $CWD'` has to
// deliver a live $CWD for the inner shell to expand on every tick, not this
// shell's directory frozen at the moment you pressed return.
//
// The quote characters themselves are COPIED THROUGH, never removed. Removal
// is parse()'s job two stages later, and split_pipeline() sits in between
// still needing them to tell `grep "a|b"` from a real pipeline.
static int is_name_start(char c)
{
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}
static int is_name_char(char c)
{
	return is_name_start(c) || (c >= '0' && c <= '9');
}

// ── positional parameters (2026-08-22) ──────────────────────────────────────
// A script's arguments: $0 is the script's path, $1..$9 its arguments, $* all
// of them space-joined, $# how many. Empty when unset, as a Bourne shell
// would (and as an interactive husk always is — these exist for scripts).
// The sigil is `$`, because husk already expands $CWD and $NAME with it and a
// second sigil would be the wart: Chris's ruling, and his DOS reflex (%1)
// lost to his own "I always hated having to surround a variable with %%".
static const char *g_script_path = NULL;   // $0 — non-NULL means script mode
static char **g_params = NULL;             // $1.. — argv slice, kernel-owned
static int g_nparams = 0;                  // $#

// Append a substituted value, marking every byte as expansion-origin.
// False means it did not all fit — the caller turns that into a refusal
// rather than a silently shortened command.
static bool expand_append(char *dst, int *n, int cap, uint8_t *mask, const char *v)
{
	for (; v != NULL && *v != '\0'; v++)
	{
		if (*n >= cap - 1)
			return false;
		if (mask != NULL)
			mask[*n] = 1;
		dst[(*n)++] = *v;
	}
	return true;
}

// ── command substitution ────────────────────────────────────────────────────
// `$(command)` becomes what the command printed. Bourne's 1977 spelling was
// the backquote, which cannot nest and quotes badly; Korn's 1983 `$(...)`
// fixed both and POSIX took it in 1992. husk takes Korn's and skips the
// backquote entirely.
//
// The inner line runs IN THIS SHELL — through the same run_line, one
// expansion depth down — so it sees the same $1..$9, the same environment
// and the same $?. Two consequences, both deliberate (DIVERGENCES § The
// shell): a builtin inside a substitution acts on THIS shell (`$(cd /x)`
// moves you — Bourne forks a subshell and isolates it), and the
// substitution's own status is nowhere: $? after a line is that line's.
// Output is captured by run_pipeline into this depth's context; trailing
// newlines are stripped (`$(pwd)` is a word, not a line); and every
// captured byte is marked as expansion-origin, so a `|` or `>` a program
// printed can never become syntax here — the rule PR #28 settled.
// `exit` inside a substitution ends the substitution, not the shell.
//
// The inner line's exit status is handed UP to the line being expanded
// (substRan/substStatus in its context), where one consumer reads it: a
// bare assignment, `x=$(cmd)`, answers with cmd's status — the value IS
// the command there, so `$?` reporting "the assignment worked" would be
// information-free. A command that merely contains a substitution keeps
// its own status (`echo $(false)` is echo's 0): `$?` names the program
// you just watched, not one buried in its arguments. Bourne's rule, 1977.
static bool run_substitution(const char *inner, int last_status,
                             const char **out, int *outLen)
{
	if (s_expdepth >= SUBST_DEPTH_MAX)
	{
		err_puts("husk: $( nested too deep - not run\n");
		s_expand_complained = true;
		return false;
	}
	char line[LINE_MAX];
	os64_strcopy(line, sizeof(line), inner);    // run_line tokenizes in place

	s_expdepth++;
	expansion_ctx_t *ctx = &s_expctx[s_expdepth];
	ctx->captureLen = 0;
	ctx->captureOverflow = false;
	int status = last_status;
	(void)run_line(line, &status);
	s_expdepth--;
	s_expctx[s_expdepth].substRan = true;
	s_expctx[s_expdepth].substStatus = status;

	if (ctx->captureOverflow)
	{
		err_puts("husk: $( produced more than a line can hold - not run\n");
		s_expand_complained = true;
		return false;
	}
	*out = ctx->capture;
	*outLen = ctx->captureLen;
	return true;
}

// Returns false if the expansion did not fit in `dst` — see run_segment.
static bool expand_line(const char *src, char *dst, int cap, int last_status,
                        uint8_t *mask)
{
	int n = 0;
	char quote = 0;              // 0, '\'' or '"' — which quote we are inside
	bool fit = true;

	if (mask != NULL)
		for (int i = 0; i < cap; i++)
			mask[i] = 0;
	s_expctx[s_expdepth].substRan = false;   // this segment's substitutions start from none

	while (*src && n < cap - 1)
	{
		// Quote bookkeeping first, so every test below can simply ask
		// "are we inside single quotes?" and be done with it.
		if (quote == 0 && (*src == '\'' || *src == '"'))
		{
			quote = *src;
			dst[n++] = *src++;
			continue;
		}
		if (quote != 0 && *src == quote)
		{
			quote = 0;
			dst[n++] = *src++;
			continue;
		}

		if (quote == '\'')
			dst[n++] = *src++;   // inside '': the shell is deaf, on purpose
		else if (src[0] == '$' && src[1] == '(')
		{
			const char *close = find_subst_close(src + 2);
			if (close == NULL)
			{
				err_puts("husk: unterminated $( - not run\n");
				s_expand_complained = true;
				return false;
			}
			// The inner text is handed over RAW: the line that runs it does
			// its own expanding. Expanding it here first would hand a
			// substituted value to a second parser as syntax.
			int ilen = (int)(close - (src + 2));
			char inner[LINE_MAX];
			if (ilen >= LINE_MAX)
			{
				err_puts("husk: $( too long - not run\n");
				s_expand_complained = true;
				return false;
			}
			for (int i = 0; i < ilen; i++)
				inner[i] = src[2 + i];
			inner[ilen] = '\0';

			const char *out;
			int outLen;
			if (!run_substitution(inner, last_status, &out, &outLen))
				return false;
			while (outLen > 0 && out[outLen - 1] == '\n')
				outLen--;                       // Bourne strips trailing newlines
			for (int i = 0; i < outLen; i++)
			{
				if (n >= cap - 1)
				{
					fit = false;
					break;
				}
				if (mask != NULL)
					mask[n] = 1;
				dst[n++] = out[i];
			}
			src = close + 1;
		}
		else if (src[0] == '$' && src[1] == '?')
		{
			char nb[24];
			utoa((unsigned long)(unsigned int)last_status, nb);   // NUL-terminates
			if (!expand_append(dst, &n, cap, mask, nb))
				fit = false;
			src += 2;
		}
		else if (src[0] == '$' && src[1] == '$')
		{
			char nb[24];
			utoa((unsigned long)os64_taskid(), nb);
			if (!expand_append(dst, &n, cap, mask, nb))
				fit = false;
			src += 2;
		}
		else if (src[0] == '$' && src[1] >= '0' && src[1] <= '9')
		{
			// $0..$9 — ONE digit, Bourne's rule (${10} is a door that stays
			// closed until someone writes a script with ten arguments).
			int idx = src[1] - '0';
			const char *v = NULL;
			if (idx == 0)
				v = g_script_path;
			else if (idx <= g_nparams)
				v = g_params[idx - 1];
			if (!expand_append(dst, &n, cap, mask, v))
				fit = false;
			src += 2;
		}
		else if (src[0] == '$' && src[1] == '*')
		{
			for (int i = 0; i < g_nparams; i++)
			{
				// The joining space is generated too, so it is marked with
				// the rest: `$*` is one substitution, not a line of syntax.
				if (i > 0 && !expand_append(dst, &n, cap, mask, " "))
					fit = false;
				if (!expand_append(dst, &n, cap, mask, g_params[i]))
					fit = false;
			}
			src += 2;
		}
		else if (src[0] == '$' && src[1] == '#')
		{
			char nb[24];
			utoa((unsigned long)g_nparams, nb);
			if (!expand_append(dst, &n, cap, mask, nb))
				fit = false;
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
				if (os64_getcwd(cwd, sizeof(cwd)) >= 0 &&
				    !expand_append(dst, &n, cap, mask, cwd))
					fit = false;
			}
			else
			{
				// An environment value is substituted text like any other,
				// and has always been: `PATH` holding a `|` was this same
				// bug long before $1 existed.
				if (!expand_append(dst, &n, cap, mask, os64_getenv(name)))
					fit = false;
			}
		}
		else
			dst[n++] = *src++;
	}
	dst[n] = 0;

	// The main loop stops at cap - 1 as well as at end of source; anything
	// left in `src` is a line that did not fit.
	if (*src != '\0')
		fit = false;
	return fit;
}

// ── child lifecycle trace ───────────────────────────────────────────────────
// Husk knows more than wait() ever can: at spawn time it has both the task ID
// and the command name. Keep that pair together until reap so DEBUG_APPLICATION
// gets matching bookends — especially for pipelines, where a row of bare task
// numbers is otherwise a logic puzzle. os64_debug_log supplies the debug-level
// gate; when application logging is off, these calls cost no log output.
static void launched_task_set(launched_task_t *task, long tid,
                              const char *path)
{
	const char *name = path;
	for (const char *p = path; *p != '\0'; p++)
		if (*p == '/' && p[1] != '\0')
			name = p + 1;

	task->tid = tid;
	os64_strcopy(task->name, sizeof(task->name), name);
}

static void report_start(const launched_task_t *task)
{
	char msg[128];
	os64_snprintf(msg, sizeof(msg), "husk: task %lu (%s) started",
	              (unsigned long)task->tid, task->name);
	os64_debug_log(msg);
}

static void report_exit(const launched_task_t *task, int code)
{
	char msg[160];
	os64_snprintf(msg, sizeof(msg), "husk: task %lu (%s) exited code %lu",
	              (unsigned long)task->tid, task->name,
	              (unsigned long)(unsigned int)code);
	os64_debug_log(msg);
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
	launched_task_t tasks[MAX_STAGES]; // every stage — `a | b &` is ONE job
	int  remaining;              // stages not yet collected
	long reportTid;              // the LAST stage's task: the job's public identity
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


// "[1] 57" — job number and task number both, on purpose. In bash the PID is
// noise because you kill a job by `%1`; os64 has no such notation, so the TASK
// NUMBER is the handle you actually use: `echo kill > /proc/57/ctl`. Printing
// it is load-bearing here, not decoration.
static void job_announce(int jobNum, long tid)
{
	os64_write(1, "[", 1);
	put_num((unsigned long)jobNum);
	os64_write(1, "] ", 2);
	put_num((unsigned long)tid);
	os64_write(1, "\n", 1);
}

static void job_report_done(const job_t *j)
{
	os64_write(1, "[", 1);
	put_num((unsigned long)j->jobNum);
	os64_write(1, "]+ ", 3);
	put_num((unsigned long)j->reportTid);
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
static int job_add(const launched_task_t *tasks, int taskCount)
{
	for (int i = 0; i < MAX_JOBS; i++)
	{
		if (gJobs[i].used)
			continue;
		gJobs[i].used      = 1;
		gJobs[i].jobNum    = gNextJobNum++;
		gJobs[i].remaining = taskCount;
		gJobs[i].lastCode  = 0;
		for (int k = 0; k < MAX_STAGES; k++)
			gJobs[i].tasks[k].tid = -1;
		for (int k = 0; k < taskCount; k++)
			gJobs[i].tasks[k] = tasks[k];
		gJobs[i].reportTid = tasks[taskCount - 1].tid; // last stage speaks for the job
		return gJobs[i].jobNum;
	}
	return 0;
}

// A child just came back from reap(). If it belongs to a tracked job, count it
// off; when the job's last stage is collected, announce it. Returns 1 if the
// task was ours to account for.
static int job_note_reaped(long tid, int code)
{
	for (int i = 0; i < MAX_JOBS; i++)
	{
		if (!gJobs[i].used)
			continue;
		for (int k = 0; k < MAX_STAGES; k++)
		{
			if (gJobs[i].tasks[k].tid != tid)
				continue;
			report_exit(&gJobs[i].tasks[k], code);
			gJobs[i].tasks[k].tid = -1;        // collected
			if (tid == gJobs[i].reportTid)
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
		long tid = os64_reap(&code);
		if (tid <= 0)
			break;              // 0 = nobody has died; that is the usual answer
		job_note_reaped(tid, code);
	}
}

// ── the prompt ──────────────────────────────────────────────────────────────
//
// `$PROMPT`, and with it unset the prompt is "husk> " exactly as it has always
// been. THE NAME IS PROMPT AND NOT PS1 because the 1 in PS1 only means
// anything alongside PS2, PS3 and PS4 — continuation, select, xtrace — and
// husk has none of them, so the number points at siblings that do not exist.
// (PATH keeps its name for the opposite reason: it is a good one.)
//
// ESCAPES ONLY. THE PROMPT IS NEVER RE-EXPANDED, and that is a ruling rather
// than an omission: bash runs PS1 through parameter AND command substitution
// on every single print, which is its most famous foot-gun. Here the string is
// rendered by walking a fixed vocabulary that cannot introduce syntax, and
// anything else you want is an ordinary expansion done ONCE, at the
// assignment, through the path PR #28 settled — expansions are DATA, never
// syntax. That is also why there is no \h for the hostname:
// `export PROMPT="$HOSTNAME:\w $ "` already works, and brings every other
// environment variable with it for free.
//
//   \w  working directory          \?  the last command's exit status
//   \W  its last component         \j  background jobs being tracked
//   \t  time, HH:MM:SS             \n  newline, for a two-line prompt
//   \d  date, YYYY-MM-DD           \\  a literal backslash
//
// The date is ISO because os64 has no locale to have an opinion with, and
// because the one format that sorts is the one worth picking. Both clock
// escapes read local time through libos64's calendar, so they honour $TZ like
// everything else.
//
// ABSENT, each because os64 has nothing to put in it: \u (there are no users),
// \h (above), \$'s #-for-root (there are no privilege levels), and \[ \] (the
// console has no ANSI escape parser, so there are no zero-width sequences to
// guard — and husk's rub-out counts typed characters rather than columns, so
// prompt width is not load-bearing either). \! is available the day anyone
// wants it; the history ring is right up there.
//
// AN ESCAPE THE VOCABULARY DOES NOT KNOW IS EMITTED VERBATIM, backslash and
// all. A prompt is cosmetic, so refusing to draw one over a typo would be
// hostile — and a `\q` sitting in your prompt on every line is a louder
// tripwire than a message you would see once and scroll past.
#define PROMPT_MAX 256

static void prompt_put(char *out, int *n, const char *s)
{
	while (*s != '\0' && *n < PROMPT_MAX - 1)
		out[(*n)++] = *s++;
}

// Zero-padded two digits — the clock reads wrong without the padding, and
// every field that uses it is 0..59 or 1..31.
static void prompt_put_2(char *out, int *n, int v)
{
	if (*n < PROMPT_MAX - 1) out[(*n)++] = (char)('0' + (v / 10) % 10);
	if (*n < PROMPT_MAX - 1) out[(*n)++] = (char)('0' + v % 10);
}

static void prompt_put_num(char *out, int *n, unsigned long v)
{
	char b[24];
	utoa(v, b);            // NUL-terminates
	prompt_put(out, n, b);
}

static void prompt_render(int last_status)
{
	const char *fmt = os64_getenv("PROMPT");
	if (fmt == NULL || *fmt == '\0')
	{
		os64_write(1, "husk> ", 6);
		return;
	}

	char out[PROMPT_MAX];
	int n = 0;

	while (*fmt != '\0' && n < PROMPT_MAX - 1)
	{
		// A trailing lone backslash is a backslash; there is nothing after it
		// to name an escape.
		if (*fmt != '\\' || fmt[1] == '\0')
		{
			out[n++] = *fmt++;
			continue;
		}

		char code = fmt[1];
		fmt += 2;
		switch (code)
		{
			case 'w':
			case 'W':
			{
				char cwd[256];
				if (os64_getcwd(cwd, sizeof(cwd)) < 0)
					break;              // no answer: show nothing, not garbage

				const char *show = cwd;
				if (code == 'W')
				{
					const char *slash = NULL;
					for (const char *p = cwd; *p != '\0'; p++)
						if (*p == '/')
							slash = p;
					if (slash != NULL && slash[1] != '\0')
						show = slash + 1;
					else if (slash == cwd)
						show = "/";     // the root has no last component, and
						                // a prompt with a hole where the
						                // directory goes is worse than a "/"
				}
				prompt_put(out, &n, show);
				break;
			}

			case 't':
			case 'd':
			{
				os64_date_t d;
				if (os64_date_now(&d, NULL) != 0)
					break;              // same rule as the cwd: no answer, no text
				if (code == 't')
				{
					prompt_put_2(out, &n, d.hour);
					prompt_put(out, &n, ":");
					prompt_put_2(out, &n, d.minute);
					prompt_put(out, &n, ":");
					prompt_put_2(out, &n, d.second);
				}
				else
				{
					prompt_put_num(out, &n, (unsigned long)d.year);
					prompt_put(out, &n, "-");
					prompt_put_2(out, &n, d.month);
					prompt_put(out, &n, "-");
					prompt_put_2(out, &n, d.day);
				}
				break;
			}

			// Spelled exactly as $? is (run_expanded stores it the same way),
			// so the two never disagree about what a negative status looks
			// like.
			case '?':
				prompt_put_num(out, &n, (unsigned long)(unsigned int)last_status);
				break;

			case 'j':
			{
				int live = 0;
				for (int i = 0; i < MAX_JOBS; i++)
					if (gJobs[i].used)
						live++;
				prompt_put_num(out, &n, (unsigned long)live);
				break;
			}

			case 'n':  out[n++] = '\n'; break;
			case '\\': out[n++] = '\\'; break;

			default:
				// Unknown: give both characters back, so the typo is visible.
				out[n++] = '\\';
				if (n < PROMPT_MAX - 1)
					out[n++] = code;
				break;
		}
	}

	// Truncation at PROMPT_MAX is silent because it is not silent: a prompt
	// that stops mid-word is its own error message, on every line.
	os64_write(1, out, (unsigned)n);
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
	if (byte_is_expanded(&line[n - 1]))
		return 0;               // substituted text, not an operator
	if (n >= 2 && line[n - 2] != ' ' && line[n - 2] != '\t')
		return 0;               // part of a token, not an operator
	n--;                        // drop the '&' itself
	while (n > 0 && (line[n - 1] == ' ' || line[n - 1] == '\t'))
		n--;
	line[n] = '\0';
	return 1;
}

// Where a child's three streams go, as the redirection tokens said. Filled
// by extract_redirections, spent by run_pipeline.
typedef struct {
	char *inFile;                // `< file`
	char *outFile;               // `> file` / `>> file` / `&> file` / `&>> file`
	int   outAppend;
	char *errFile;               // `2> file` / `2>> file`
	int   errAppend;
	bool  errJoinsOut;           // `2>&1` (and `&>`): stderr goes where stdout ENDS UP
	bool  outJoinsErr;           // `>&2`: stdout goes where stderr ends up
} redirections_t;

// Pull the redirection tokens out of an already-parsed argv, compacting what
// remains. Returns 0 with *r describing the streams, or -1 on a dangling
// operator (`upper <` with no filename), or on `>&2 2>&1` — each stream sent
// to the other is a circle with no file in it.
//
// The operators must be their own tokens — husk's parser splits on spaces
// only, and that simplicity is a feature (`upper<f` is a program named
// "upper<f", which is honest, if unhelpful). Token-exact matching also
// means `>>` needs no lexer priority over `>` — they arrive as different
// whole tokens and can never shadow each other.
//
// THE STDERR VOCABULARY. The digit is V7 Bourne's file-descriptor number
// (Thompson's shell had only bare `<` and `>`); `&>` is bash's 1989
// shorthand for "both". Per stream the LAST spelling wins, and a file
// spelling and a join spelling replace each other: `2> f 2>&1` joins,
// `2>&1 2> f` writes f.
//
// `2>&1` MEANS WHAT EVERYONE THINKS IT MEANS: stderr goes wherever stdout
// finally goes — a file, a pipe, the console — no matter where on the line
// it was written. Bourne resolves it POSITIONALLY (stderr gets whatever
// stdout was bound to at the moment the token was read), which is why
// `cmd 2>&1 > f` leaves stderr on the terminal there and why every shell
// tutorial has a paragraph about it. That paragraph is the fossil; the
// intent never was. (Chris's ruling, 2026-08-28: "the other way just
// sounds annoying." DIVERGENCES § The shell.)
static int extract_redirections(char *cargv[], redirections_t *r)
{
	r->inFile = NULL;
	r->outFile = NULL;
	r->outAppend = 0;
	r->errFile = NULL;
	r->errAppend = 0;
	r->errJoinsOut = false;
	r->outJoinsErr = false;

	int w = 0;
	for (int i = 0; cargv[i]; i++)
	{
		// A token is an OPERATOR only if the author typed every byte of it.
		// `echo $1` with $1 = "> passwd" prints it, the way every shell since
		// 1977 has.
		//
		// EVERY byte, and not just the first — checking only the first left
		// two ways in, both found by re-reading this fix as its own reviewer
		// (2026-08-23). `>$x` with $x = ">" is one token that husk would
		// normally treat as a literal (its operators are token-exact, so
		// `>foo` is an argument), and the substitution turned it into the
		// APPEND operator: data creating syntax by completing it. And a
		// GLOB result is data too — it comes from the filesystem, which is
		// the outside world by another door — so a file innocently named ">"
		// made `echo *` redirect into whatever filename sorted after it.
		if (!token_is_typed(cargv[i]))
		{
			cargv[w++] = cargv[i];
			continue;
		}
		const char *t = cargv[i];
		if (str_eq(t, "<"))
		{
			if (!cargv[i + 1]) return -1;
			r->inFile = cargv[++i];
		}
		else if (str_eq(t, ">") || str_eq(t, ">>"))
		{
			if (!cargv[i + 1]) return -1;
			r->outFile = cargv[++i];
			r->outAppend = (t[1] == '>');
			r->outJoinsErr = false;
		}
		else if (str_eq(t, "2>") || str_eq(t, "2>>"))
		{
			if (!cargv[i + 1]) return -1;
			r->errFile = cargv[++i];
			r->errAppend = (t[2] == '>');
			r->errJoinsOut = false;
		}
		else if (str_eq(t, "&>") || str_eq(t, "&>>"))
		{
			if (!cargv[i + 1]) return -1;
			r->outFile = cargv[++i];
			r->outAppend = (t[2] == '>');
			r->outJoinsErr = false;
			r->errFile = NULL;
			r->errJoinsOut = true;
		}
		else if (str_eq(t, "2>&1"))
		{
			r->errFile = NULL;
			r->errJoinsOut = true;
		}
		else if (str_eq(t, ">&2") || str_eq(t, "1>&2"))
		{
			r->outFile = NULL;
			r->outJoinsErr = true;
		}
		else
		{
			cargv[w++] = cargv[i];
		}
	}
	cargv[w] = 0;
	if (r->errJoinsOut && r->outJoinsErr)
		return -1;                // each stream sent to the other: a circle
	return w == 0 ? -1 : 0;       // a line that was ALL redirections has no program
}

static bool s_interactive;        // set by main before the prompt loop: the keyboard is ours

// Build and run a pipeline: spawn every stage, wiring stage i's stdout to
// stage i+1's stdin through a pipe. The last stage keeps the console —
// or, inside a `$(...)`, writes into the capture pipe.
//
// Returns the pipeline's exit status for $?: the LAST stage's exit code —
// the same answer the Bourne shell has given since 1977, and the sensible
// one: the last stage is the program whose output you just watched. A
// bad redirection reports 1 and runs nothing; a stage that cannot be
// spawned reports 1 unless a LATER stage ran and answered (its complaint
// went where its stderr was sent, and the plumbing after it still runs).
//
// THE CLOSE DISCIPLINE IS THE WHOLE JOB. Every end husk hands to a child, husk
// must then close its OWN copy of — because the reader downstream sees
// end-of-input only when the LAST write end closes. Keep husk's copy of a write
// end open and that reader waits forever for an EOF that can never come: the
// classic `a | b` hang that every hand-written shell suffers exactly once. The
// child already holds its own reference, so closing ours takes nothing from it.
static int run_pipeline(char *stages[], int nstages, int background)
{
	launched_task_t tasks[MAX_STAGES];
	int stageOf[MAX_STAGES];    // which stage each launched task was
	int taskCount = 0;
	int prev_read = -1;         // read end of the pipe from the PREVIOUS stage
	int status = 0;             // what $? will remember of this line
	int failedStage = -1;       // the last stage that could not be spawned

	// Inside a `$(...)` the LAST stage's stdout is captured through a pipe
	// of this pipeline's own (a `> f` written inside the substitution still
	// wins the slot — it is a redirect like any other). One pipe per
	// pipeline, drained to EOF below BEFORE the children are reaped: wait
	// first and a child with more than a pipe's worth to say blocks on a
	// full pipe while husk blocks on the wait — the classic capture deadlock.
	int capPipe[2] = { -1, -1 };
	if (s_expdepth > 0 && os64_pipe(capPipe) < 0)
	{
		err_puts("husk: out of pipes\n");
		return 1;
	}

	for (int i = 0; i < nstages; i++)
	{
		char *cargv[ARGS_MAX];
		int cargc = parse(stages[i], cargv, ARGS_MAX);
		if (cargc < 0)
		{
			// A wildcard matched nothing (or overflowed): glob_expand has
			// already said exactly what and why. Refusing the whole line is
			// the ruling — a pattern that matched nothing never becomes a
			// filename here.
			status = 1;
			break;
		}
		if (cargc == 0)
		{
			err_puts("husk: empty command in pipeline\n");
			status = 1;
			break;
		}

		// Redirections come out of argv before the child ever sees it —
		// `upper < in > out` runs upper with argc == 1, exactly as if the
		// shell had been reading and writing the files itself.
		redirections_t redir;
		if (extract_redirections(cargv, &redir) < 0)
		{
			err_puts("husk: bad redirection (expected `< file`, `> file`, `>> file`, "
			         "`2> file`, `2>> file`, `2>&1`, `>&2`, `&> file` or `&>> file`)\n");
			status = 1;
			break;
		}

		// Open redirect files BEFORE creating the pipe — if the file isn't
		// there, we want to fail while there's nothing yet to unwind.
		// `>>` opens "a": position at the end, create if absent — both
		// filesystems already spoke append (FAT since its glue was born,
		// ext2 since the write arc); the shell was the last one to learn.
		int inRedir = -1, outRedir = -1, errRedir = -1;
		if (redir.inFile && (inRedir = (int)os64_open(redir.inFile, "r")) < 0)
		{
			err_puts("husk: cannot open ");
			err_puts(redir.inFile);
			err_puts("\n");
			status = 1;
			break;
		}
		if (redir.outFile &&
		    (outRedir = (int)os64_open(redir.outFile, redir.outAppend ? "a" : "w")) < 0)
		{
			err_puts("husk: cannot create ");
			err_puts(redir.outFile);
			err_puts("\n");
			if (inRedir >= 0) os64_close(inRedir);
			status = 1;
			break;
		}
		if (redir.errFile &&
		    (errRedir = (int)os64_open(redir.errFile, redir.errAppend ? "a" : "w")) < 0)
		{
			err_puts("husk: cannot create ");
			err_puts(redir.errFile);
			err_puts("\n");
			if (inRedir >= 0)  os64_close(inRedir);
			if (outRedir >= 0) os64_close(outRedir);
			status = 1;
			break;
		}

		// A pipe to the NEXT stage — the last stage doesn't need one.
		int p[2] = { -1, -1 };
		if (i < nstages - 1 && os64_pipe(p) < 0)
		{
			err_puts("husk: out of pipes\n");
			if (inRedir >= 0)  os64_close(inRedir);
			if (outRedir >= 0) os64_close(outRedir);
			if (errRedir >= 0) os64_close(errRedir);
			status = 1;
			break;
		}

		// Slot priority: an explicit redirect beats the pipeline's plumbing.
		int in  = (inRedir  >= 0) ? inRedir  : prev_read;    // -1: console
		int out = (outRedir >= 0) ? outRedir
		        : (i < nstages - 1) ? p[1] : capPipe[1];     // -1: console
		int err = (errRedir >= 0) ? errRedir : -1;           // -1: console
		// The joins bind LAST, to the other stream's final destination —
		// which is what makes `cmd 2>&1 | grep` carry both streams into the
		// pipe. The two are mutually exclusive (extract_redirections refuses
		// the pair), so there is no order to get wrong here. A `>&2` in a
		// non-final stage sends stdout to the console and the pipe's write
		// end is closed unused below, so the next stage reads EOF: the
		// output went where it was sent. The kernel refcounts each slot on
		// its own, so handing one handle to two slots is an ordinary share.
		if (redir.errJoinsOut) err = out;
		if (redir.outJoinsErr) out = err;

		// PATH resolution happens HERE, at spawn time — argv[0] stays the
		// name as typed (a program is told what it was called, not where it
		// was found; that's how busybox-style tricks stay possible someday).
		char pathbuf[256];
		const char *prog = os64_resolve_command(cargv[0], pathbuf, sizeof(pathbuf));

		// The BACKGROUND flag rides all the way to task_create, because the
		// kernel has to know before the child's first instruction: a background
		// job's read of handle 0 returns EOF instead of competing with husk for
		// the keyboard. Output is untouched — it still prints to the screen.
		long tid = os64_spawn_redirected(prog, cargv, in, out, err,
		                                 background ? OS64_SPAWN_BACKGROUND : 0);

		if (tid < 0)
		{
			// The complaint is ABOUT this command, so it goes where this
			// command's stderr was sent — `bogus 2> /dev/null` stays quiet,
			// and a `2>&1` into a pipe carries it downstream. A forking shell
			// gets this by accident (its child fails after the redirects are
			// in place); husk, which fails in the parent, does it on purpose.
			// Written BEFORE the hand-off closes below, while the handle is
			// still ours.
			int where = (err >= 0) ? err : OS64_STDERR;
			hputs(where, "husk: cannot run ");
			hputs(where, cargv[0]);
			hputs(where, "\n");
		}

		// Hand-off done — drop husk's copies of EVERYTHING it just passed
		// along (or displaced). The displaced case matters: if a redirect won
		// slot 0 over prev_read, husk still holds the pipe's read end, and
		// closing it is what tells the upstream writer its reader is gone.
		if (prev_read >= 0) { os64_close(prev_read); prev_read = -1; }
		if (p[1] >= 0)        os64_close(p[1]);
		if (inRedir >= 0)     os64_close(inRedir);
		if (outRedir >= 0)    os64_close(outRedir);
		if (errRedir >= 0)    os64_close(errRedir);

		if (tid < 0)
		{
			// A stage that cannot start is a stage that produced nothing; the
			// rest of the plumbing still runs. Its output pipe (closed on the
			// write side just above) hands the next stage an immediate EOF —
			// after the complaint, when a `2>&1` sent that down the pipe.
			// The verdict is 1 unless a LATER stage overwrites it, which is
			// the pipeline rule: the last stage answers for the line.
			status = 1;
			failedStage = i;
			prev_read = p[0];
			continue;
		}

		launched_task_set(&tasks[taskCount], tid, prog);
		report_start(&tasks[taskCount]);
		stageOf[taskCount] = i;
		taskCount++;
		prev_read = p[0];       // this stage's output becomes the next one's input
	}

	if (prev_read >= 0)
		os64_close(prev_read);  // belt and braces: never leave an end dangling

	if (capPipe[1] >= 0)
		os64_close(capPipe[1]); // EOF can only arrive once husk's own copy is gone
	if (capPipe[0] >= 0)
	{
		// Drain to EOF, which arrives when the last writer closes — normally
		// the last stage exiting. A background job inside a substitution
		// holds the write end too, so the substitution waits for it; that is
		// what Bourne does as well, and `$(x &)` is nobody's idiom. Past the
		// capacity the bytes are read and dropped so the writer can finish,
		// and the overflow refuses the substitution afterwards.
		expansion_ctx_t *ctx = &s_expctx[s_expdepth];
		for (;;)
		{
			char chunk[512];
			int64_t got = os64_read(capPipe[0], chunk, sizeof(chunk));
			if (got <= 0)
				break;              // EOF, or an interruption treated as one
			int room = EXPANDED_MAX - 1 - ctx->captureLen;
			int take = (got < room) ? (int)got : room;
			for (int k = 0; k < take; k++)
				ctx->capture[ctx->captureLen + k] = chunk[k];
			ctx->captureLen += take;
			if (take < got)
				ctx->captureOverflow = true;
		}
		os64_close(capPipe[0]);
	}

	// A BACKGROUND job is the one case where husk does not wait: it hands the
	// pipeline to the job table, prints "[1] 57", and goes straight back to the
	// prompt. The corpses are collected later by jobs_poll(). $? is 0 — the
	// LAUNCH succeeded; the job's own status arrives with "[1]+ 57 Done", which
	// is the only honest answer when the thing hasn't finished yet.
	if (background && taskCount > 0)
	{
		int jobNum = job_add(tasks, taskCount);
		if (jobNum > 0)
			job_announce(jobNum, tasks[taskCount - 1].tid);
		return status;
	}

	// Reap every child we launched. They run CONCURRENTLY — that is the point
	// of a pipeline; stage 2 is already chewing on stage 1's first bytes long
	// before stage 1 finishes. We just collect the corpses in order.
	int interrupted = 0;
	for (int i = 0; i < taskCount; i++)
	{
		int code = 0;
		long ended;
		// A caught signal (a resize, once husk has a handler for one) cuts the
		// wait short with NOTHING collected — the child still lives and still
		// owns the console. Returning to the prompt here would be a shell
		// talking over its own foreground job; the child is still ours to
		// wait for, so wait again (SIGNALS.md §8: no restart, the caller
		// loops — this is the loop).
		do
			ended = os64_wait(tasks[i].tid, &code);
		while (ended == OS64_INTERRUPTED);
		if (ended > 0)
			report_exit(&tasks[i], code);
		if (code == 130)        // 128 + SIGINT: died by Ctrl+C
			interrupted = 1;
		if (stageOf[i] > failedStage)
			status = code;      // reaped in launch order, so the last stage wins —
			                    // unless the last stage was the one that never ran
	}

	// Echo the interrupt ONCE, after the whole pipeline is collected — the
	// victim was probably mid-line on the console, and this is the visible
	// answer to the keystroke (a real terminal echoes ^C at the keypress; we
	// echo at the funeral, ~10ms later, which reads the same to a human).
	// The INTERACTIVE husk's job: a script's husk answers 130 to whoever
	// ran it, and that shell echoes — two echoes for one keystroke otherwise.
	if (interrupted && s_interactive)
		os64_write(1, "^C\n", 3);

	return status;
}

// ── one line, interpreted ───────────────────────────────────────────────────

// Print `time`'s verdict: "time: 12.34s". The ruler is the 10ms tick, so
// hundredths are exactly as honest as the clock gets — the microsecond
// version arrives with the wall-clock-hardening arc, same as ping's.
static void print_elapsed(unsigned long dticks, unsigned long per_second)
{
	unsigned long secs = 0, hundredths = 0;
	if (per_second > 0)
	{
		secs = dticks / per_second;
		hundredths = (dticks % per_second) * 100 / per_second;
	}
	os64_write(1, "time: ", 6);
	put_num(secs);
	os64_write(1, ".", 1);
	if (hundredths < 10)
		os64_write(1, "0", 1);
	put_num(hundredths);
	os64_write(1, "s\n", 2);
}

// ── the directory stack ─────────────────────────────────────────────────────
// `pushd DIR` remembers where you were and goes to DIR; `popd` goes back;
// `dirs` shows the stack; a bare `pushd` swaps you with the top. Bill Joy's
// csh (1978) — the first shell to notice that "go there, then come back" is
// a stack, not a pair of cds. `cd -` is ksh's (1983) one-deep cousin: back
// to the directory you were in before this one, no stack involved.
//
// Entries are PATH STRINGS, captured live from the kernel at push time, so
// a directory renamed underneath the stack strands its entry exactly the
// way it strands a task's cwd (DEBTS § cwd-as-string). A `popd` into an
// entry that no longer resolves complains and KEEPS the entry: dropping it
// silently would lose the one clue to where you had been.
#define DIR_STACK_DEPTH 16
#define DIR_PATH_MAX    256

static char s_dirstack[DIR_STACK_DEPTH][DIR_PATH_MAX];
static int  s_dirdepth = 0;
static char s_prevdir[DIR_PATH_MAX];     // `cd -`'s target
static bool s_have_prevdir = false;

// Print the stack the way csh does: one line, where you are first, then
// the entries newest to oldest — a bare `pushd` swaps the first two words.
static void dirs_print(void)
{
	char cwd[DIR_PATH_MAX];
	if (os64_getcwd(cwd, sizeof(cwd)) >= 0)
		os64_write(1, cwd, os64_strlen(cwd));
	for (int i = s_dirdepth - 1; i >= 0; i--)
	{
		os64_write(1, " ", 1);
		os64_write(1, s_dirstack[i], os64_strlen(s_dirstack[i]));
	}
	os64_write(1, "\n", 1);
}

// Go to `dest`, remembering where you were for `cd -`. Returns 0, or 1
// after complaining — the number is the builtin's $?.
static int change_dir(const char *dest)
{
	char here[DIR_PATH_MAX];
	bool haveHere = os64_getcwd(here, sizeof(here)) >= 0;
	if (os64_chdir(dest) < 0)
	{
		err_puts("husk: cd: no such directory: ");
		err_puts(dest);
		err_puts("\n");
		return 1;
	}
	if (haveHere)
	{
		os64_strcopy(s_prevdir, sizeof(s_prevdir), here);
		s_have_prevdir = true;
	}
	return 0;
}

// The directory builtins, already parsed into argv. Returns the $? verdict.
static int run_dir_builtin(char *cargv[], int cargc)
{
	const char *verb = cargv[0];

	if (str_eq(verb, "cd"))
	{
		// `cd` alone goes to the root — there's no $HOME to go home to.
		const char *dest = (cargc > 1) ? cargv[1] : "/";
		if (cargc > 1 && str_eq(cargv[1], "-") && token_is_typed(cargv[1]))
		{
			if (!s_have_prevdir)
			{
				err_puts("husk: cd: no previous directory\n");
				return 1;
			}
			// The target is overwritten by the move itself, so copy first.
			char back[DIR_PATH_MAX];
			os64_strcopy(back, sizeof(back), s_prevdir);
			if (change_dir(back) != 0)
				return 1;
			// An implicit destination is announced, as ksh does — you did
			// not type where you were going.
			os64_write(1, back, os64_strlen(back));
			os64_write(1, "\n", 1);
			return 0;
		}
		return change_dir(dest);
	}

	if (str_eq(verb, "dirs"))
	{
		dirs_print();
		return 0;
	}

	if (str_eq(verb, "popd"))
	{
		if (s_dirdepth == 0)
		{
			err_puts("husk: popd: directory stack empty\n");
			return 1;
		}
		if (change_dir(s_dirstack[s_dirdepth - 1]) != 0)
			return 1;            // entry kept — see the header comment
		s_dirdepth--;
		dirs_print();
		return 0;
	}

	// pushd
	char here[DIR_PATH_MAX];
	if (os64_getcwd(here, sizeof(here)) < 0)
	{
		err_puts("husk: pushd: cannot read the current directory\n");
		return 1;
	}
	if (cargc == 1)
	{
		// Bare pushd: swap places with the top entry.
		if (s_dirdepth == 0)
		{
			err_puts("husk: pushd: directory stack empty\n");
			return 1;
		}
		char top[DIR_PATH_MAX];
		os64_strcopy(top, sizeof(top), s_dirstack[s_dirdepth - 1]);
		if (change_dir(top) != 0)
			return 1;
		os64_strcopy(s_dirstack[s_dirdepth - 1], DIR_PATH_MAX, here);
		dirs_print();
		return 0;
	}
	if (s_dirdepth >= DIR_STACK_DEPTH)
	{
		err_puts("husk: pushd: directory stack full\n");
		return 1;
	}
	// Move FIRST, push second: a pushd into nowhere leaves the stack as it
	// was, and $? says so.
	if (change_dir(cargv[1]) != 0)
		return 1;
	os64_strcopy(s_dirstack[s_dirdepth++], DIR_PATH_MAX, here);
	dirs_print();
	return 0;
}

// `NAME=value`, with `p` at the `=`. The value is the rest of the statement
// with its TYPED quotes removed, exactly as parse() would strip them, but
// WITHOUT globbing (`X=*` sets a star, as it has in every Bourne shell) and
// as ONE word: a value with unquoted blanks is refused, and so is the
// `NAME=value command` form — a per-command environment is env(1)'s job
// (`env NAME=value command`: a program that changes its own copy and runs
// the command), and refusing the spelling here beats silently running the
// command with the variable set in the SHELL.
static int run_assignment(const char *stmt, const char *eq, int *last_status)
{
	char name[64];
	int k = 0;
	for (const char *q = stmt; q < eq && k < (int)sizeof(name) - 1; q++)
		name[k++] = *q;
	name[k] = '\0';

	char value[EXPANDED_MAX];
	int n = 0;
	char quote = 0;
	for (const char *v = eq + 1; *v != '\0'; v++)
	{
		bool typed = !byte_is_expanded(v);
		if (quote != 0)
		{
			if (*v == quote && typed)
			{
				quote = 0;
				continue;
			}
		}
		else if ((*v == '\'' || *v == '"') && typed)
		{
			quote = *v;
			continue;
		}
		else if ((*v == ' ' || *v == '\t') && typed)
		{
			const char *q = v;
			while (*q == ' ' || *q == '\t')
				q++;
			if (*q == '\0')
				break;                    // trailing blanks are nothing
			err_puts("husk: an assignment takes one word - quote a value with spaces "
			         "(for one command only: env NAME=value command)\n");
			*last_status = 2;
			return 0;
		}
		if (n < (int)sizeof(value) - 1)
			value[n++] = *v;
	}
	value[n] = '\0';
	if (quote != 0)
	{
		err_puts("husk: unterminated quote in assignment\n");
		*last_status = 2;
		return 0;
	}
	if (os64_setenv(name, value) != 0)
	{
		err_puts("husk: cannot set ");
		err_puts(name);
		err_puts(" (environment full?)\n");
		*last_status = 1;
		return 0;
	}
	// `x=$(cmd)` answers with cmd's status (run_substitution says why); a
	// plain `x=value` has nothing to report but that it worked.
	expansion_ctx_t *ctx = &s_expctx[s_expdepth];
	*last_status = ctx->substRan ? ctx->substStatus : 0;
	return 0;
}

// Run one ALREADY-EXPANDED command: the trailing `&`, the pipeline split,
// the builtins, and finally run_pipeline. Returns 1 when the command asks
// the shell to exit, else 0; the status lands in *last_status ($?).
//
// THE RETURN VALUE IS EXIT-OR-NOT, NOT A STATUS, and the two look identical
// at a glance — which is how `cd /nomatch*`, `unset [zz]` and
// `export FOO=[zz]` came to KILL THE SHELL (found 2026-08-28, driving the new
// $PROMPT in with an unquoted assignment). Each of the three had a
// glob-failure path that wanted "stop, do not fall through to the bare-verb
// default", reached for `return 1` meaning "return early", and got "quit"
// instead. A builtin that failed reports through *last_status and returns 0.
// Only `exit` returns 1.
static int run_expanded(char *expanded, int *last_status)
{
	char *stages[MAX_STAGES];

	// A trailing `&` applies to its WHOLE command, not the last stage:
	// `hello | upper &` backgrounds the pipeline, which is why this runs
	// before split_pipeline rather than inside it. (And per COMMAND, not
	// per line, since `;` arrived: `slow & ; fast` backgrounds slow and
	// runs fast immediately — which is what anyone typing it meant.)
	int background = strip_background(expanded);
	if (expanded[0] == '\0')
		return 0;           // the command was nothing but an `&`

	int nstages = split_pipeline(expanded, stages, MAX_STAGES);

	// `exit` is a builtin because it touches the SHELL'S OWN state (its
	// lifetime) — no separate program could ever do it. Checked WITHOUT
	// parse(), which tokenizes in place and would eat the line before
	// run_pipeline ever saw the arguments.
	if (nstages == 1 && first_token_is(stages[0], "exit"))
		return 1;

	// NAME=value — a variable. A VARIABLE IS AN ENVIRONMENT VARIABLE (Chris's
	// ruling, 2026-08-28: one namespace — DIVERGENCES § The shell). Bourne's
	// 1977 split, private unless exported, was a PDP-11 economy (the block is
	// copied at every exec) that became a silent trap: a child that cannot
	// see a value says nothing, so everyone learns to export everything and
	// pays for a distinction they never use. Here an assignment IS the
	// export; `export NAME=value` is the same act under its older name; and
	// $NAME at expansion time reads what either wrote. NAME and the `=` must
	// be TYPED — a $x holding `A=B` is a word of data, not an assignment.
	if (nstages == 1)
	{
		const char *s = stages[0];
		while (*s == ' ' || *s == '\t')
			s++;
		const char *p = s;
		if (is_name_start(*p) && !byte_is_expanded(p))
		{
			while (is_name_char(*p) && !byte_is_expanded(p))
				p++;
			if (*p == '=' && !byte_is_expanded(p))
				return run_assignment(s, p, last_status);
		}
	}

	// `export` is a builtin by the same physics as cd: the environment
	// copies DOWNWARD at spawn, never sideways or up, so an external
	// export would set its own copy and take it to the grave. Since the
	// one-namespace ruling above it is an assignment wearing its 1977
	// name; it stays because fingers type it. `export` alone lists the
	// environment, same walk env(1) does.
	if (nstages == 1 && first_token_is(stages[0], "export"))
	{
		char *eargv[ARGS_MAX];
		int eargc = parse(stages[0], eargv, ARGS_MAX);
		if (eargc < 0)
		{
			*last_status = 1;   // glob already reported; do NOT fall into "list"
			return 0;   // 0 = keep the shell (see the contract above)
		}
		*last_status = 0;
		if (eargc < 2)
		{
			// Bare `export`: list. The block is mapped right here in
			// our address space — walking it costs no syscalls.
			// One line per variable exactly as env(1) prints it — the value
			// by POINTER (os64_env_next hands back a bounded copy, and a
			// $(...) capture can be longer than the bound), with its control
			// bytes spelled as escapes (a captured `ls` carries newlines,
			// and a listing is only a listing while one line is one
			// variable).
			os64_envent_t e = { .index = 0 };
			while (os64_env_next(&e) == 0)
			{
				const char *v = os64_getenv(e.key);
				os64_puts(e.key);
				os64_puts("=");
				os64_write_escaped(OS64_STDOUT, v != NULL ? v : "");
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
				err_puts("husk: export: expected KEY=VALUE\n");
				*last_status = 1;
			}
			else
			{
				*eq = 0;   // split in place; parse() already owns the line
				if (os64_setenv(eargv[1], eq + 1) != 0)
				{
					err_puts("husk: export: failed (environment full?)\n");
					*last_status = 1;
				}
			}
		}
		return 0;
	}

	// `unset` — export's undo, builtin by the same one-way valve.
	// Unsetting the absent is success (idempotent since Bourne).
	if (nstages == 1 && first_token_is(stages[0], "unset"))
	{
		char *uargv[ARGS_MAX];
		int uargc = parse(stages[0], uargv, ARGS_MAX);
		if (uargc < 0)
		{
			*last_status = 1;   // glob already reported; not an "expected a KEY" error
			return 0;   // 0 = keep the shell (see the contract above)
		}
		if (uargc < 2)
		{
			err_puts("husk: unset: expected a KEY\n");
			*last_status = 1;
		}
		else
		{
			*last_status = 0;
			for (int u = 1; u < uargc; u++)
				if (os64_unsetenv(uargv[u]) != 0)
					*last_status = 1;
		}
		return 0;
	}

	// `cd` is THE canonical builtin — the textbook answer to "why must
	// any command be built in?": an external cd would change ITS OWN
	// cwd (a copy inherited at spawn) and take the change to its grave.
	// Only the shell can move the shell. Its relatives `pushd`, `popd`
	// and `dirs` are builtins by the same physics.
	if (nstages == 1 && (first_token_is(stages[0], "cd") ||
	                     first_token_is(stages[0], "pushd") ||
	                     first_token_is(stages[0], "popd") ||
	                     first_token_is(stages[0], "dirs")))
	{
		char *cargv[ARGS_MAX];
		int cargc = parse(stages[0], cargv, ARGS_MAX);
		if (cargc < 0)
		{
			// A failed glob must NOT fall through to the bare-`cd` default:
			// `cd /nomatch*` would silently take you to / , which is the most
			// alarming thing a shell can do quietly.
			*last_status = 1;
			return 0;   // 0 = keep the shell (see the contract above)
		}
		*last_status = run_dir_builtin(cargv, cargc);
		return 0;
	}

	*last_status = run_pipeline(stages, nstages, background);
	return 0;
}

// Run one command from the list: $-expand it (HERE, per command, so
// `false ; echo $?` prints 1 — see split_commands), honor a `time` prefix,
// and hand the rest to run_expanded.
//
// `time` is a builtin by the vantage-point rule (the sharpened criterion:
// a builtin is justified exactly when the command needs the shell's own
// state or the shell's own POSITION). An external time can only measure
// what it can spawn — one program. It structurally cannot time a pipeline,
// a builtin, or a sequence; only the party standing at both ends of the
// whole job — before dispatch, after the last reap — can. History ran the
// experiment: V7 shipped /usr/bin/time, csh pulled it inside in 1978
// because the external kept lying about pipelines, and POSIX finally made
// `time` a reserved word prefixing a whole pipeline. Measurement lives
// where orchestration lives. (os32 had both; its gut said the builtin
// "felt more solid" — forty years of consensus arriving early.)
static int run_segment(char *seg, int *last_status)
{
	// This depth's context (see expansion_ctx_t): a `$(...)` in the segment
	// runs its inner line one depth down, in the next context, and comes
	// back here with the output.
	expansion_ctx_t *ctx = &s_expctx[s_expdepth];
	char *expanded = ctx->text;

	s_expand_complained = false;
	if (!expand_line(seg, expanded, EXPANDED_MAX, *last_status, ctx->mask))
	{
		// Refuse rather than run the part that fit. The prefix of an expanded
		// line is a different command — the same reasoning that makes an
		// over-long script line a refusal (line_overflowed). A substitution
		// that failed has already said why.
		if (!s_expand_complained)
			err_puts("husk: line too long after expansion - not run\n");
		*last_status = 1;
		return 0;
	}
	s_expbase = expanded;
	s_expmask = ctx->mask;
	s_explen  = (int)os64_strlen(expanded);

	if (first_token_is(expanded, "time"))
	{
		char *rest = expanded;
		while (*rest == ' ') rest++;         // the token itself
		rest += 4;                            // "time"
		while (*rest == ' ' || *rest == '\t') rest++;
		if (*rest == '\0')
		{
			err_puts("husk: time: nothing to time\n");
			*last_status = 1;
			return 0;
		}

		os64_ticks_t t0, t1;
		os64_ticks(&t0);
		int exiting = run_expanded(rest, last_status);
		os64_ticks(&t1);
		// Timing a `&` job measures the LAUNCH (~0.00s) — honest, if
		// unenlightening; the job's own duration ends at "[1]+ Done",
		// which no prefix can see. $? passes through untouched: time
		// reports on the clock, never on the verdict.
		print_elapsed((unsigned long)(t1.ticks - t0.ticks), t0.per_second);
		return exiting;
	}

	// `!` negates the verdict — Bourne's, and the only way to say "if this
	// FAILS" without a program that exists to fail. A prefix like `time`,
	// for the same vantage-point reason: it reports on the whole pipeline.
	if (first_token_is(expanded, "!"))
	{
		char *rest = expanded;
		while (*rest == ' ') rest++;
		rest += 1;                            // "!"
		while (*rest == ' ' || *rest == '\t') rest++;
		if (*rest == '\0')
		{
			err_puts("husk: !: nothing to negate\n");
			*last_status = 1;
			return 0;
		}
		int exiting = run_expanded(rest, last_status);
		*last_status = (*last_status == 0) ? 1 : 0;
		return exiting;
	}

	return run_expanded(expanded, last_status);
}

// Everything a line MEANS, top level: split on `;` / `&&` / `||`, then run
// each command in order with Bourne's short-circuit rules — `&&` runs its
// right side only if the left succeeded, `||` only if it failed, `;`
// unconditionally. A SKIPPED command leaves $? untouched, so
// `false && a || b` runs b: the || tests false's status straight through
// a's absence, exactly as every shell since 1977 has chained them.
// Interactive lines and husk.rc lines both come through here, and that is
// the entire point — the rc file is not a configuration format, it IS the
// shell, so anything that works at the prompt works there by construction.
// Returns 1 when any command asks the shell to exit (the rest of the line
// dies with the shell — what could it print to?), else 0.
static int run_line(char *line, int *last_status)
{
	char *cmds[MAX_CMDS];
	int seps[MAX_CMDS];

	int ncmds = split_commands(line, cmds, seps, MAX_CMDS);
	if (ncmds < 0)
	{
		err_puts("husk: too many commands on one line (limit 8)\n");
		*last_status = 1;
		return 0;
	}

	for (int i = 0; i < ncmds; i++)
	{
		if (i > 0)
		{
			if (seps[i - 1] == SEP_AND && *last_status != 0)
				continue;
			if (seps[i - 1] == SEP_OR && *last_status == 0)
				continue;
		}
		if (run_segment(cmds[i], last_status))
			return 1;
	}
	return 0;
}

// ── control flow: if / while ────────────────────────────────────────────────
// Thompson's shell had no syntax for this at all: `/bin/if` and `/bin/goto`
// were PROGRAMS (goto seeked the script file). Bourne made them syntax in
// 1977 and borrowed Algol 68's closers — fi, done, esac — writing the shell
// in C macros that made it look like Algol. husk takes the syntax and keeps
// the test OUTSIDE it: `if` decides on an exit status, which is all a shell
// should know; `test`, `true` and `false` are programs (V7 shipped /bin/test
// as one in 1979, and `[` was a link to it).
//
//   if LIST          while LIST       for NAME in WORDS
//   then …           do …             do …
//   elif LIST        done             done
//   then …
//   else …
//   fi
//
// A LIST is any command line; its last status decides (0 = true). `then`,
// `do`, `else` may follow a `;` on the same line, or open their own. `!`
// negates (run_segment). `for` assigns each word to NAME — an environment
// variable, os64's only kind (run_assignment). Every keyword lives in
// s_keywords, once; `in` is not one, it is read by exec_for alone.
//
// THE ASSEMBLER AND THE EVALUATOR. Every line source — keyboard, rc, script,
// -c — hands physical lines to feed_line, which cuts them into STATEMENTS at
// top-level `;` and feeds each. A statement outside a block runs at once, as
// it always has. `if`/`while` opens a block: statements are collected until
// the matching `fi`/`done` closes it, then the whole block is run by the
// evaluator, which walks the collected statements recursively. Collected,
// because a loop body runs more than once and a branch may not run at all —
// neither can be executed as it is read.
//
// A STATEMENT LIMIT AND A DEPTH LIMIT, both loud (BLOCK_STMTS_MAX,
// BLOCK_DEPTH_MAX). Ctrl+C: a child that died of it (130) aborts the block
// it was in, and a loop polls the console between iterations for one typed
// while husk itself was foreground (loop_interrupted). Interactive only —
// a script's stdin may be data, and polling it would eat that data.
// `break`/`continue`/`case` are booked (DEBTS § husk scripting).
enum { KW_NONE, KW_IF, KW_THEN, KW_ELIF, KW_ELSE, KW_FI, KW_WHILE, KW_DO, KW_DONE, KW_FOR, KW_COUNT };
static const char *const s_keywords[] = {
	"", "if", "then", "elif", "else", "fi", "while", "do", "done", "for"
};

// The first word of `s` as a keyword (KW_NONE if it is not one), with *rest
// pointing past it and its blanks. A keyword is the WHOLE word: `iffy` is a
// program.
static int keyword_of(const char *s, const char **rest)
{
	while (*s == ' ' || *s == '\t')
		s++;
	for (int k = KW_IF; k < KW_COUNT; k++)
	{
		const char *w = s_keywords[k];
		const char *p = s;
		while (*w != '\0' && *p == *w) { p++; w++; }
		if (*w == '\0' && (*p == '\0' || *p == ' ' || *p == '\t'))
		{
			while (*p == ' ' || *p == '\t')
				p++;
			if (rest != NULL)
				*rest = p;
			return k;
		}
	}
	if (rest != NULL)
		*rest = s;
	return KW_NONE;
}

#define BLOCK_STMTS_MAX 128
#define BLOCK_DEPTH_MAX 8

typedef struct {
	char stmts[BLOCK_STMTS_MAX][LINE_MAX];
	int  lineOf[BLOCK_STMTS_MAX];   // physical line each came from (0 = keyboard)
	int  count;
	int  depth;                     // blocks opened and not yet closed
	int  startLine;                 // where the outermost one began
} block_t;

static block_t s_block;
static bool    s_block_abort;       // the evaluator is unwinding: error, exit, or Ctrl+C

static void block_reset(void)
{
	s_block.count = 0;
	s_block.depth = 0;
}

static void block_complain(const char *msg, int lineNo)
{
	err_puts("husk: ");
	err_puts(msg);
	if (lineNo > 0)
	{
		err_puts(" (line ");
		err_num((unsigned long)lineNo);
		err_puts(")");
	}
	err_puts("\n");
}

// Run one statement through run_line, on a copy: run_line tokenizes in place
// and a loop body runs more than once. A child killed by Ctrl+C (130) aborts
// the block — the Bourne shell's SIGINT ends the compound it was in, not
// merely the command.
static int run_stmt(const char *stmt, int *last_status)
{
	char copy[LINE_MAX];
	os64_strcopy(copy, sizeof(copy), stmt);
	int exiting = run_line(copy, last_status);
	if (*last_status == 130)
		s_block_abort = true;
	return exiting;
}

// Between iterations of a loop. A Ctrl+C typed while husk itself is the
// foreground task arrives as a DATA byte (the console only signals a
// foreground CHILD), so poll for it with zero patience; anything else found
// there was typed ahead and is parked for the next prompt.
static bool loop_interrupted(void)
{
	char c;
	while (os64_read_for(0, &c, 1, 0) == 1)
	{
		if (c == 0x03)
		{
			os64_write(1, "^C\n", 3);
			return true;
		}
		pushback_put(c);
	}
	return false;
}

// The first statement in [from, to) whose keyword is in `wanted` (a bitmask
// of 1 << KW_x), AT THIS NESTING LEVEL — nested if/while blocks are stepped
// over whole. -1 if the range ends first.
static int find_kw(int from, int to, unsigned wanted)
{
	int level = 0;
	for (int i = from; i < to; i++)
	{
		int kw = keyword_of(s_block.stmts[i], NULL);
		if (level == 0 && (wanted & (1u << kw)))
			return i;
		if (kw == KW_IF || kw == KW_WHILE || kw == KW_FOR)
			level++;
		else if (kw == KW_FI || kw == KW_DONE)
			level--;
	}
	return -1;
}

static int exec_stmts(int from, int to, int *last_status);

// A condition: the text after the opening keyword, then the statements up to
// `then`/`do`. The LAST status decides. Returns the exiting verdict.
static int run_cond(const char *rest, int from, int to, int *last_status)
{
	int exiting = 0;
	if (*rest != '\0')
		exiting = run_stmt(rest, last_status);
	for (int i = from; i < to && !exiting && !s_block_abort; i++)
		exiting = run_stmt(s_block.stmts[i], last_status);
	return exiting;
}

// `if` at statement i. Returns the index after its `fi`, and the exiting
// verdict through *exiting. On a malformed block, complains, aborts.
static int exec_if(int i, int to, int *last_status, int *exiting)
{
	const char *rest;
	keyword_of(s_block.stmts[i], &rest);
	int thenIdx = find_kw(i + 1, to, 1u << KW_THEN);
	int fiIdx   = (thenIdx < 0) ? -1 : find_kw(thenIdx + 1, to, 1u << KW_FI);
	if (thenIdx < 0 || fiIdx < 0)
	{
		block_complain(thenIdx < 0 ? "if without then" : "if without fi", s_block.lineOf[i]);
		s_block_abort = true;
		return to;
	}

	int condFrom = i + 1, condTo = thenIdx, bodyFrom = thenIdx + 1;
	bool taken = false;
	for (;;)
	{
		int next = find_kw(bodyFrom, fiIdx, (1u << KW_ELIF) | (1u << KW_ELSE));
		int bodyTo = (next < 0) ? fiIdx : next;
		if (!taken)
		{
			*exiting = run_cond(rest, condFrom, condTo, last_status);
			if (*exiting || s_block_abort)
				return to;
			if (*last_status == 0)
			{
				taken = true;
				*exiting = exec_stmts(bodyFrom, bodyTo, last_status);
				if (*exiting || s_block_abort)
					return to;
			}
		}
		if (next < 0)
			break;
		if (keyword_of(s_block.stmts[next], &rest) == KW_ELSE)
		{
			if (!taken)
			{
				*exiting = exec_stmts(next + 1, fiIdx, last_status);
				if (*exiting || s_block_abort)
					return to;
			}
			break;
		}
		// elif: a fresh condition, then its body
		int thenAgain = find_kw(next + 1, fiIdx, 1u << KW_THEN);
		if (thenAgain < 0)
		{
			block_complain("elif without then", s_block.lineOf[next]);
			s_block_abort = true;
			return to;
		}
		condFrom = next + 1;
		condTo = thenAgain;
		bodyFrom = thenAgain + 1;
	}
	return fiIdx + 1;
}

// `while` at statement i. Returns the index after its `done`.
static int exec_while(int i, int to, int *last_status, int *exiting)
{
	const char *rest;
	keyword_of(s_block.stmts[i], &rest);
	int doIdx   = find_kw(i + 1, to, 1u << KW_DO);
	int doneIdx = (doIdx < 0) ? -1 : find_kw(doIdx + 1, to, 1u << KW_DONE);
	if (doIdx < 0 || doneIdx < 0)
	{
		block_complain(doIdx < 0 ? "while without do" : "while without done", s_block.lineOf[i]);
		s_block_abort = true;
		return to;
	}
	for (;;)
	{
		if (s_interactive && loop_interrupted())
		{
			*last_status = 130;       // the same verdict as a child Ctrl+C killed
			s_block_abort = true;
			return to;
		}
		*exiting = run_cond(rest, i + 1, doIdx, last_status);
		if (*exiting || s_block_abort)
			return to;
		if (*last_status != 0)
			break;
		*exiting = exec_stmts(doIdx + 1, doneIdx, last_status);
		if (*exiting || s_block_abort)
			return to;
	}
	// A loop that ran to its end succeeded: $? is not the failed test that
	// stopped it (Bourne agrees — the status is the last BODY command's, or 0).
	*last_status = 0;
	return doneIdx + 1;
}

// A `for` keeps its expanded word list here for the loop's lifetime: the
// expansion buffer and the glob pool are reused by every statement the body
// runs, so the list cannot live where parse() left it. Nested loops stack
// their lists; a list that does not fit is refused, never truncated.
#define FOR_POOL_BYTES (64 * 1024)
static char s_for_pool[FOR_POOL_BYTES];
static int  s_for_used = 0;

// `for` at statement i: `for NAME in WORDS` walks the words, `for NAME` walks
// a script's arguments (Bourne's 1977 shapes, both). The list is expanded
// ONCE, before the first iteration — globs, $NAME and $(...) included — and
// each word is assigned the way any assignment is: into the environment,
// where the body's $NAME reads it and the body's children inherit it. NAME
// keeps its last value afterwards. Returns the index after `done`.
static int exec_for(int i, int to, int *last_status, int *exiting)
{
	const char *rest;
	keyword_of(s_block.stmts[i], &rest);
	int doIdx   = find_kw(i + 1, to, 1u << KW_DO);
	int doneIdx = (doIdx < 0) ? -1 : find_kw(doIdx + 1, to, 1u << KW_DONE);
	if (doIdx < 0 || doneIdx < 0)
	{
		block_complain(doIdx < 0 ? "for without do" : "for without done", s_block.lineOf[i]);
		s_block_abort = true;
		return to;
	}

	char name[64];
	int k = 0;
	const char *p = rest;
	if (!is_name_start(*p))
	{
		block_complain("for: expected a variable name", s_block.lineOf[i]);
		s_block_abort = true;
		return to;
	}
	while (is_name_char(*p) && k < (int)sizeof(name) - 1)
		name[k++] = *p++;
	name[k] = '\0';
	while (*p == ' ' || *p == '\t')
		p++;

	int poolStart = s_for_used;
	char *words[ARGS_MAX];
	int nwords = 0;
	if (*p == '\0')
	{
		for (int j = 0; j < g_nparams && nwords < ARGS_MAX - 1; j++)
			words[nwords++] = g_params[j];
	}
	else
	{
		if (!(p[0] == 'i' && p[1] == 'n' && (p[2] == ' ' || p[2] == '\t' || p[2] == '\0')))
		{
			block_complain("for: expected `in` after the variable name", s_block.lineOf[i]);
			s_block_abort = true;
			return to;
		}
		p += 2;
		// This depth's context is idle between statements, so the list can
		// be expanded in it — and must then be copied out (see s_for_pool).
		expansion_ctx_t *ctx = &s_expctx[s_expdepth];
		s_expand_complained = false;
		if (!expand_line(p, ctx->text, EXPANDED_MAX, *last_status, ctx->mask))
		{
			if (!s_expand_complained)
				block_complain("for: list too long after expansion", s_block.lineOf[i]);
			s_block_abort = true;
			return to;
		}
		s_expbase = ctx->text;
		s_expmask = ctx->mask;
		s_explen  = (int)os64_strlen(ctx->text);
		char *argv[ARGS_MAX];
		int argc = parse(ctx->text, argv, ARGS_MAX);
		if (argc < 0)
		{
			s_block_abort = true;         // the glob has already said why
			return to;
		}
		for (int j = 0; j < argc; j++)
		{
			int len = (int)os64_strlen(argv[j]) + 1;
			if (s_for_used + len > FOR_POOL_BYTES)
			{
				block_complain("for: word list too long", s_block.lineOf[i]);
				s_for_used = poolStart;
				s_block_abort = true;
				return to;
			}
			os64_strcopy(s_for_pool + s_for_used, (size_t)len, argv[j]);
			words[nwords++] = s_for_pool + s_for_used;
			s_for_used += len;
		}
	}

	for (int w = 0; w < nwords; w++)
	{
		if (s_interactive && loop_interrupted())
		{
			*last_status = 130;
			s_block_abort = true;
			break;
		}
		if (os64_setenv(name, words[w]) != 0)
		{
			block_complain("for: cannot set the variable (environment full?)", s_block.lineOf[i]);
			s_block_abort = true;
			break;
		}
		*exiting = exec_stmts(doIdx + 1, doneIdx, last_status);
		if (*exiting || s_block_abort)
			break;
	}
	s_for_used = poolStart;
	if (*exiting || s_block_abort)
		return to;
	if (nwords == 0)
		*last_status = 0;                 // nothing ran, nothing failed
	return doneIdx + 1;
}

// Statements [from, to): plain ones run, compounds recurse. Returns the
// exiting verdict.
static int exec_stmts(int from, int to, int *last_status)
{
	int exiting = 0;
	int i = from;
	while (i < to && !exiting && !s_block_abort)
	{
		int kw = keyword_of(s_block.stmts[i], NULL);
		if (kw == KW_IF)
			i = exec_if(i, to, last_status, &exiting);
		else if (kw == KW_WHILE)
			i = exec_while(i, to, last_status, &exiting);
		else if (kw == KW_FOR)
			i = exec_for(i, to, last_status, &exiting);
		else if (kw == KW_NONE)
		{
			exiting = run_stmt(s_block.stmts[i], last_status);
			i++;
		}
		else
		{
			block_complain("unexpected keyword", s_block.lineOf[i]);
			err_puts("  ");
			err_puts(s_block.stmts[i]);
			err_puts("\n");
			s_block_abort = true;
		}
	}
	return exiting;
}

// The block is complete: run it, then forget it.
static int run_block(int *last_status)
{
	s_block_abort = false;
	int exiting = exec_stmts(0, s_block.count, last_status);
	if (s_block_abort && *last_status != 130)
		*last_status = 2;             // a block that could not run is a syntax verdict
	block_reset();
	return exiting;
}

// One statement, already cut from its line. Outside a block it runs at once;
// `if`/`while` opens one; inside, it is collected. Returns exiting.
static int feed_stmt(char *stmt, int lineNo, int *last_status)
{
	while (*stmt == ' ' || *stmt == '\t')
		stmt++;
	if (*stmt == '\0')
		return 0;

	const char *rest;
	int kw = keyword_of(stmt, &rest);

	// `then cmd`, `else cmd`, `do cmd` — the keyword is a statement of its
	// own and the command is the next one (it may itself be an `if`).
	if ((kw == KW_THEN || kw == KW_ELSE || kw == KW_DO) && *rest != '\0')
	{
		char head[8];
		os64_strcopy(head, sizeof(head), s_keywords[kw]);
		int exiting = feed_stmt(head, lineNo, last_status);
		if (exiting || s_block.depth == 0)
			return exiting;           // a stray `then` was refused; do not run its tail
		char tail[LINE_MAX];
		os64_strcopy(tail, sizeof(tail), rest);
		return feed_stmt(tail, lineNo, last_status);
	}
	if ((kw == KW_FI || kw == KW_DONE) && *rest != '\0')
	{
		block_complain("text after the closing keyword", lineNo);
		block_reset();
		*last_status = 2;
		return 0;
	}

	if (s_block.depth == 0)
	{
		if (kw == KW_NONE)
			return run_line(stmt, last_status);
		if (kw != KW_IF && kw != KW_WHILE && kw != KW_FOR)
		{
			block_complain("unexpected keyword outside a block", lineNo);
			err_puts("  ");
			err_puts(stmt);
			err_puts("\n");
			*last_status = 2;
			return 0;
		}
		s_block.startLine = lineNo;
	}

	if (s_block.count >= BLOCK_STMTS_MAX)
	{
		block_complain("block too long - abandoned", s_block.startLine);
		block_reset();
		*last_status = 2;
		return 0;
	}
	os64_strcopy(s_block.stmts[s_block.count], LINE_MAX, stmt);
	s_block.lineOf[s_block.count] = lineNo;
	s_block.count++;

	if (kw == KW_IF || kw == KW_WHILE || kw == KW_FOR)
	{
		if (++s_block.depth > BLOCK_DEPTH_MAX)
		{
			block_complain("blocks nested too deep - abandoned", lineNo);
			block_reset();
			*last_status = 2;
			return 0;
		}
	}
	else if (kw == KW_FI || kw == KW_DONE)
	{
		if (--s_block.depth == 0)
			return run_block(last_status);
	}
	return 0;
}

// A physical line from any source: cut at top-level `;` (quotes and `$()`
// hide theirs) and feed the pieces. Returns the exiting verdict. Feeding
// `a` then `b` is what `a; b` always meant: each expands at its own turn.
static int feed_line(char *line, int lineNo, int *last_status)
{
	char *start = line;
	char *p = line;
	char quote = 0;
	int exiting = 0;
	for (;;)
	{
		char c = *p;
		if (c == '\0')
			return exiting ? exiting : feed_stmt(start, lineNo, last_status);
		if (quote != 0)
		{
			if (c == quote)
				quote = 0;
			p++;
		}
		else if (c == '\'' || c == '"')
			quote = *p++;
		else if (c == '$' && p[1] == '(')
		{
			const char *close = find_subst_close(p + 2);
			p = (close != NULL) ? (char *)close + 1 : p + os64_strlen(p);
		}
		else if (c == ';')
		{
			*p = '\0';
			exiting = feed_stmt(start, lineNo, last_status);
			if (exiting)
				return exiting;       // `exit`: the rest of the line dies with the shell
			start = ++p;
		}
		else
			p++;
	}
}

// The prompt as the shell is showing it right now: inside an open
// `if`/`while`/`for`, Bourne's `> ` continuation — the shell is still
// collecting, not waiting for a command — otherwise $PROMPT rendered
// against the last status main recorded. One function, because two things
// draw it: main before each line, and the completion listing when it
// repaints the line it interrupted.
static int s_prompt_status = 0;

static void prompt_draw(void)
{
	if (s_block.depth > 0)
		os64_write(1, "> ", 2);
	else
		prompt_render(s_prompt_status);
}

// At the end of an rc, a script or a -c line: a block still open is a
// mistake the author should hear about, not silently run. Says so and
// forgets it; true if there was one.
static bool block_unterminated(const char *what, const char *path, int *last_status)
{
	if (s_block.depth == 0)
		return false;
	err_puts("husk: ");
	if (path != NULL)
	{
		err_puts(path);
		err_puts(": ");
	}
	err_puts(what);
	err_puts(" ended inside an unfinished if/while");
	if (s_block.startLine > 0)
	{
		err_puts(" (opened at line ");
		err_num((unsigned long)s_block.startLine);
		err_puts(")");
	}
	err_puts(" - not run\n");
	block_reset();
	*last_status = 2;
	return true;
}

// ── the rc file ─────────────────────────────────────────────────────────────
// "rc" is runcom — "run commands" — Louis Pouzin's word from CTSS in the
// early 1960s, and he coined "shell" for the same idea; both predate Unix by
// most of a decade. The mechanism has never changed: a file of commands the
// shell runs on itself at startup, exactly as if a very fast user had typed
// them. etc/husk.rc (the build's copy) carries the full story.
//
// The search order is a persistence gradient, first hit wins. Since
// 2026-08-23 the first half of it belongs to the SYSTEM, not to this file:
//   husk.rc         wherever the CONFIG SEARCH PATH says (/etc/os64.conf's
//                   `conf =`, default /home then /etc) — YOUR copy on its own
//                   partition, surviving builds and root refreshes untouched
//                   (the persistence doctrine, ruled 2026-08-07: root is the
//                   system's, /home is yours), then the SYSTEM's copy on the
//                   writable ext2 root
//   /fat/husk.rc    the lifeboat's copy, rewritten onto FAT every `make`
//   /husk.rc        the same FAT file when a lifeboat boot mounts it as "/"
//
// The two lifeboat spellings stay hardcoded ON PURPOSE and stay LAST: the
// lifeboat is for the day the ext2 root is broken, and the search path's own
// root file lives on that root. A ladder readable only when the disk is
// healthy is no help when it isn't.
//
// HUSKRC is the login-shell distinction, enforced by the environment's
// one-way valve: the first husk sets it BEFORE running the file, every
// descendant inherits it and skips — so `husk` typed inside husk gets a
// clean subshell instead of a second copy of every daemon the rc started.
// (Unix split .profile from .cshrc over exactly this; the env flows only
// downward, so one variable is the whole mechanism.)

// ── an over-long line is REFUSED, never half-run ────────────────────────────
// os64_readline delivers a line longer than the buffer TRUNCATED, having
// already consumed the tail (io.h — a deliberate anti-fgets rule: you never
// get the severed tail served back to you as a line). For a file being READ
// that is the right contract. For a file being EXECUTED it is not, because
// the prefix is a DIFFERENT COMMAND from the one the author wrote: cut
// `cp a b > log` at the wrong byte and it is still a perfectly runnable line
// doing something else, and a line that loses its trailing `# comment` loses
// the comment, not the command. The -c path has refused over-long lines since
// the day it was written; the rc and scripts quietly ran the prefix.
// (Codex review, 2026-08-22.)
//
// The mechanism is one spare byte: read into LINE_MAX + 1, and a stored length
// of LINE_MAX means the line ran to at least LINE_MAX bytes — past husk's
// stated limit of LINE_MAX - 1 either way — so it is refused out loud. A line
// that fits never touches the extra byte.
static bool line_overflowed(const char *line)
{
	int n = 0;
	while (line[n] != '\0')
		n++;
	return n >= LINE_MAX;
}

// The other way a file of commands can end: not at its end. os64_readline
// answers 1 (a line), 0 (end of input) or NEGATIVE (the read or the seek-back
// under it failed) — three endings, of which only the middle one means the
// file is finished. Treating the third as the second is how a script reports
// success for work it never performed.
static void report_read_failed(const char *what, const char *path,
                               int lineNo, int64_t err)
{
	err_puts("husk: ");
	err_puts(path);
	err_puts(": read failed after line ");
	err_num((unsigned long)lineNo);
	err_puts(" (error ");
	if (err < 0)
	{
		err_puts("-");
		err_num((unsigned long)(-err));
	}
	else
		err_num((unsigned long)err);
	err_puts(") - ");
	err_puts(what);
	err_puts(" stopped\n");
}

static void report_line_too_long(const char *what, const char *path, int lineNo)
{
	err_puts("husk: ");
	err_puts(path);
	err_puts(": line ");
	err_num((unsigned long)lineNo);
	err_puts(" is longer than ");
	err_num((unsigned long)(LINE_MAX - 1));
	err_puts(" characters - refusing to run part of it (");
	err_puts(what);
	err_puts(" stopped)\n");
}

static int run_rc(int *last_status)
{
	// THE LIFEBOAT SPELLINGS, and why they are still written out here
	// (2026-08-23). The /home-then-/etc half of this list moved to the system
	// search path, /etc/os64.conf's `conf =`, along with every other reader's
	// private copy. These two did NOT, and must not: they name the FAT
	// lifeboat, which exists for the day the ext2 root is broken — and the
	// search path's own root file lives on that root. A ladder that can only
	// be read when the disk is healthy is no use on the day the disk is not,
	// so the walk happens first and these remain the fallback after it.
	static const char *rc_lifeboat[] = { "/fat/husk.rc", "/husk.rc" };

	if (os64_getenv("HUSKRC") != NULL)
		return 0;                       // a subshell: the rc already ran upstream
	os64_setenv("HUSKRC", "1");         // set BEFORE running, so rc children inherit it

	int h = -1;
	const char *found = NULL;

	char walked[OS64_CONF_PATH_MAX];
	if (os64_conf_find("husk.rc", walked, sizeof(walked)) == 0)
	{
		h = (int)os64_open(walked, "r");
		if (h >= 0)
			found = walked;
	}
	for (unsigned int i = 0; found == NULL && i < sizeof(rc_lifeboat) / sizeof(rc_lifeboat[0]); i++)
	{
		h = (int)os64_open(rc_lifeboat[i], "r");
		if (h >= 0) { found = rc_lifeboat[i]; break; }
	}
	if (found == NULL)
		return 0;                       // no rc anywhere: a plain boot, not an error

	// A breadcrumb naming WHICH copy ran — when /home and /fat disagree,
	// this line is how you find out which one the boot believed. It goes out
	// as a serial BEACON, not a plain log line, because on a LOGD= boot the
	// log lands in a file the outside world can't read until shutdown — and
	// this particular line is exactly what an outside watcher (the QEMU
	// verification harness, a human tailing the wire) needs mid-boot.
	{
		char msg[64];
		int m = 0;
		for (const char *s = "husk: rc: "; *s; s++) msg[m++] = *s;
		for (const char *s = found; *s && m < (int)sizeof(msg) - 1; s++) msg[m++] = *s;
		msg[m] = 0;
		os64_serial_log(msg);
	}

	// One line at a time through the SAME interpreter the prompt uses —
	// os64_readline strips the newline (CRLF included: a FAT file will meet
	// a Windows editor sooner or later) and truncates an over-long line at
	// LINE_MAX exactly as the prompt would have. Comments and blank lines
	// are filtered here — they are file syntax, not shell syntax (a typed
	// '#' at the prompt still means a program named '#', honestly not found).
	char line[LINE_MAX + 1];            // the spare byte — see line_overflowed
	int exiting = 0;
	int lineNo = 0;
	int64_t readVerdict = 1;            // kept, not discarded — report_read_failed
	while (!exiting && (readVerdict = os64_readline(h, line, sizeof(line))) == 1)
	{
		lineNo++;
		if (line_overflowed(line))
		{
			// The rc is the machine's own startup file, so this stops the rc
			// rather than the shell: whatever the file had already done
			// stands, and husk still reaches its prompt to be repaired from.
			report_line_too_long("rc", found, lineNo);
			break;
		}
		const char *s = line;
		while (*s == ' ' || *s == '\t')
			s++;
		if (*s == '\0' || *s == '#')
			continue;                   // blank or comment

		// `exit` in an rc is honored — the file IS the shell, and a shell
		// told to exit exits. An rc that ends this way makes husk a batch
		// interpreter, which some boot someday will want on purpose.
		exiting = feed_line(line, lineNo, last_status);
	}
	os64_close(h);

	// Same three endings as a script (report_read_failed). The rc says so and
	// husk carries on to its prompt — a machine whose startup file became
	// unreadable is exactly a machine you want a prompt on.
	if (readVerdict < 0)
		report_read_failed("rc", found, lineNo, readVerdict);
	if (!exiting)
		block_unterminated("rc", found, last_status);   // said its piece; the prompt follows
	return exiting;
}

// ── -c: one command line, from argv ─────────────────────────────────────────
// `husk -c "ps -e | grep husk"` — run ONE line and exit with its status.
//
// This is the seam that lets a PROGRAM own a command line without owning a
// parser. watch(1) is the consumer that asked for it: it has to re-run
// `ps -e | grep husk` every couple of seconds, and the only honest way to
// learn what that string means is to hand it back to the thing whose language
// it is. A command line is not a string — its meaning depends on cwd, env,
// PATH and $?, which is to say on SHELL STATE — so whoever interprets one has
// to be a shell. Unix reached that conclusion twice and never revisited it:
// system(3) is defined as "hand this string to the command processor", and
// popen(3) is sh -c with a pipe on it. Even the C standard declined to own a
// parser. (The counter-example is instructive too: make decides per recipe
// line whether it contains a metacharacter and skips the shell if not — a
// half-parser that has been quietly disagreeing with the real one for forty
// years.)
//
// Nothing architectural changes here, which is the point. run_line() has
// always been the single choke point, and the rc file already proved husk can
// execute lines nobody typed; -c adds only a third SOURCE for the string —
// keyboard, file, argv — in front of the one interpreter. The rc comment
// above called this shot: "an rc that ends this way makes husk a batch
// interpreter, which some boot someday will want on purpose."
//
// Non-interactive policy, all deliberate: no banner, no rc, no prompt, no
// history, no job table — and the exit status is the LINE's, because a caller
// that re-runs a command needs to know whether it worked. The rc is skipped
// outright rather than by the HUSKRC valve: -c is not a login shell and
// should not claim to be one.
static int run_command_argument(const char *command)
{
	char line[LINE_MAX];
	int last_status = 0;
	int i = 0;

	// Copy before running. run_line tokenizes IN PLACE — it punches NULs into
	// the separators — and the argv block is the kernel's, sized by whoever
	// spawned us rather than by anything husk chose. Refuse an over-long line
	// out loud instead of silently running the first 255 bytes of it, which
	// is the failure mode that turns `> important` into a truncated file.
	while (command[i] != '\0' && i < LINE_MAX - 1)
	{
		line[i] = command[i];
		i++;
	}
	line[i] = '\0';
	if (command[i] != '\0')
	{
		err_puts("husk: -c line too long (limit 255)\n");
		return 2;
	}

	os64_debug_log("husk: -c (one line, non-interactive)");
	feed_line(line, 0, &last_status);   // `exit` in a -c line just ends it early
	if (block_unterminated("-c", NULL, &last_status))
		return 2;
	return last_status;
}

// ── a script: a file of lines, with arguments ──────────────────────────────
// `husk FILE [args...]` — the third SOURCE for run_line after the keyboard
// and the rc, and the one the rc comment promised ("this same run_line fed
// from a file"). Nobody types that spelling, though: the kernel does, when
// a file whose first line is `#!/bin/husk` is spawned (elf_loader.h — the
// loader rewrites the request to `/bin/husk FILE args...`). So `get cp`
// works from the prompt because /bin/get says on line one who runs it, and
// husk here receives its own path as $0 and `cp` as $1.
//
// Same policy as -c: no banner, no rc, no prompt, no job table, and the exit
// status is the LAST LINE's — a script is a program and a program answers.
// The `#!` line is skipped by the same test that skips an rc comment; a
// script is an rc with arguments, which is what Thompson's sh scripts were
// in 1973 before Bourne gave them $1.
static int run_script(const char *path, char **params, int nparams)
{
	int h = (int)os64_open(path, "r");
	if (h < 0)
	{
		err_puts("husk: cannot open script ");
		err_puts(path);
		err_puts("\n");
		return 2;
	}

	g_script_path = path;
	g_params = params;
	g_nparams = nparams;

	os64_debug_log("husk: running a script");

	// LINE_MAX + 1, and the extra byte is the whole point — see line_overflowed.
	char line[LINE_MAX + 1];
	int last_status = 0;
	int exiting = 0;
	int lineNo = 0;
	// The readline verdict is KEPT, because 0 and -1 are different endings and
	// only one of them is the end of the file. `== 1` alone read a failed disk
	// read as "no more lines", so a script whose tail could not be read exited
	// with the status of the last line that DID run — quite possibly 0, which
	// is a script reporting success for work it never performed. (Codex
	// review, 2026-08-22.) Loop exit via `exiting` leaves this at 1: the
	// short-circuit means readline is not called on that pass.
	int64_t readVerdict = 1;
	while (!exiting && (readVerdict = os64_readline(h, line, sizeof(line))) == 1)
	{
		lineNo++;
		if (line_overflowed(line))
		{
			// A script STOPS. It is a program, and a program that cannot
			// read its own next instruction has no business guessing.
			report_line_too_long("script", path, lineNo);
			os64_close(h);
			return 2;
		}
		const char *s = line;
		while (*s == ' ' || *s == '\t')
			s++;
		if (*s == '\0' || *s == '#')
			continue;                   // blank, comment — and the #! line itself
		exiting = feed_line(line, lineNo, &last_status);   // `exit` ends the script
		// Ctrl+C ends the SCRIPT, not only the line it landed on. The kernel
		// aims it at the program running (the console follows this husk's
		// wait down to its child), which dies 130; running the next line as
		// though nothing happened would make a ten-line script cost ten
		// Ctrl+Cs. 130 goes upward too, so a script that ran this one stops.
		if (last_status == 130)
			break;
	}
	os64_close(h);
	if (!exiting && block_unterminated("script", path, &last_status))
		return 2;

	if (readVerdict < 0)
	{
		// A script that could not be READ did not succeed, whatever its last
		// line happened to return. Say where it stopped: "it worked up to
		// line 12" is the difference between a bad disk and a bad script.
		report_read_failed("script", path, lineNo, readVerdict);
		return 2;
	}
	return last_status;
}

// ── the shell ───────────────────────────────────────────────────────────────

int main(int argc, char **argv, char **envp)
{
	(void)envp;

	// A first argument that is not an option is a SCRIPT, and everything
	// after it belongs to the script — so `husk /bin/get -v cp` hands `-v`
	// to the script, not to this parser. That is sh(1)'s rule (the file
	// stops option parsing), and it is checked by hand here for exactly that
	// reason: the args parser would otherwise claim `-v` as husk's own.
	if (argc >= 2 && argv[1] != NULL && argv[1][0] != '-')
		return run_script(argv[1], argv + 2, argc - 2);

	// The only flag husk has, and the only one it should ever have:
	// everything else this shell does, it does because you typed it.
	const char *command = NULL;
	const os64_optspec_t specs[] = {
		{'c', "command", true, "run one command line and exit with its status",
		 .value_out = &command}
	};
	os64_args_t args = {0};
	os64_args_init(&args, argc, argv, specs, 1);
	args.about = "husk — the os64 shell.";
	args.details = "With no arguments husk is interactive: it runs its rc file, "
	               "then prompts. With -c it runs one line and exits with that "
	               "line's status — which is how a program borrows the shell's "
	               "language without borrowing its parser.";
	// No positionals HERE: a script operand was taken above, before this
	// parser ever saw the line (the door the old comment here said was
	// closed opened on 2026-08-22, as that comment predicted: "this same
	// run_line fed from a file"). The kernel launches husk with argc 0 and
	// argv NULL, which walks straight out of the parse loop on the first
	// index check.
	int32_t parsed = os64_args_parse(&args, "husk [-c \"command line\"] | husk FILE [args...]", NULL, 0);
	if (parsed == OS64_ARG_HELP) return 0;
	if (parsed < 0) return 2;

	if (command != NULL)
		return run_command_argument(command);

	os64_write(1, "husk - the os64 shell. `exit` to quit.\n",
	           sizeof("husk - the os64 shell. `exit` to quit.\n") - 1);
	os64_debug_log("husk: started");

	char line[LINE_MAX];
	int last_status = 0;            // $? — nothing has failed yet
	s_interactive = true;           // the keyboard is ours: loops may poll it for Ctrl+C

	// The rc runs after the banner and before the first prompt — its output
	// (job announcements, error messages) lands where a fast typist's would.
	if (run_rc(&last_status))
	{
		os64_write(1, "husk: bye\n", 10);
		os64_debug_log("husk: exiting (rc said exit)");
		return 0;
	}

	for (;;)
	{
		// Bury any background job that finished while you were typing, BEFORE
		// the prompt is drawn — so the "[1]+ 57 Done" line lands above a fresh
		// prompt instead of halfway through the one you are looking at. This
		// is also the only reaping background jobs ever get, which is exactly
		// why they never pile up as zombies.
		jobs_poll();

		// After jobs_poll, so \j counts what is still running rather than what
		// was running before this pass buried it.
		s_prompt_status = last_status;
		prompt_draw();
		int got = read_line(line, sizeof(line));
		if (got < 0)
		{
			block_reset();          // Ctrl+C abandons whatever was being collected
			continue;
		}
		if (got == 0)
			continue;

		if (feed_line(line, 0, &last_status))
			break;
	}

	os64_write(1, "husk: bye\n", 10);
	os64_debug_log("husk: exiting");
	return 0;
}
