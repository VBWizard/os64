// rawtty.c — the fixture for raw mode (SIGINT.md § Raw mode).
//
// The claim: a program can ask its terminal to stop interpreting Ctrl+C and
// Ctrl+D, receive them as the bytes 0x03 and 0x04, and can never leave the
// terminal that way — the kernel restores cooked when the program exits.
//
// Proven the way winchtest proves resize: on a pty, headless. The PARENT
// holds the master and types; the CHILD sits on the slave and reads. Acts:
//
//   1. cooked by default: a typed 0x04 is end-of-input (read returns 0)
//   2. raw on request: /proc/self/tty says so and names the holder; a typed
//      0x03 0x04 'x' arrives as three bytes (0x03 would have been SIGINT)
//   3. cooked on request: the same 0x04 is end-of-input again
//   4. reset at exit: a GRANDCHILD goes raw and exits without undoing it;
//      the child's terminal reads cooked afterwards
//   5. the boundary: the parent, which does not sit on the slave, is refused
//      a write to the child's tty file — the mode is the sitter's alone
//
// The parent must not type 0x03 before the child is raw (on a cooked pty it
// would be SIGINT at the write), so the child announces each act on its
// screen and the parent watches the grid for the word before typing.
//
// Exit codes, the ring3-fixture convention: one magic success value, a
// distinct code per step for the autopsy. The child exits with a step code
// on its own failure; the parent relays it.
//   0x2A710000  success                 0x2A710005  raw bytes wrong
//   0x2A710001  pty/spawn failed        0x2A710006  cooked again: not EOF
//   0x2A710002  default not cooked      0x2A710007  reset at exit failed
//   0x2A710003  cooked EOF not seen     0x2A710008  parent write not refused
//   0x2A710004  raw not granted/shown   0x2A710009  child died the wrong way

#include "os64/os64.h"
#include "os64/pty.h"

#define RAWTTY_OK          0x2A710000
#define STEP(n)            (0x2A710000 + (n))

#define COLS 80
#define ROWS 25

static os64_pty_header_t gHdr;
static os64_pty_cell_t   gCells[COLS * ROWS];

static void say(const char *s)
{
	os64_serial_log(s);
}

// ── The child: sits on the slave, reads, reports ────────────────────────────

static bool mode_is(bool raw, uint64_t holder)
{
	os64_tty_info_t info;
	if (os64_tty_read(&info) != 0)
		return false;
	return info.raw == raw && info.raw_task == holder;
}

static int child(void)
{
	uint8_t buf[8];

	// Act 1: cooked by default; the parent types 0x04, we see EOF.
	if (!mode_is(false, 0))
		return STEP(2);
	os64_printf("ACT1\n");
	if (os64_read(0, buf, sizeof(buf)) != 0)
		return STEP(3);

	// Act 2: raw on request — the file says so and names us.
	if (os64_tty_set_raw(true) != 0 || !mode_is(true, os64_taskid()))
		return STEP(4);
	os64_printf("ACT2\n");
	int64_t got = 0;
	while (got < 3)
	{
		int64_t n = os64_read(0, buf + got, sizeof(buf) - (size_t)got);
		if (n <= 0)
			return STEP(5);
		got += n;
	}
	if (got != 3 || buf[0] != 0x03 || buf[1] != 0x04 || buf[2] != 'x')
		return STEP(5);

	// Act 3: cooked on request; the same 0x04 is EOF again.
	if (os64_tty_set_raw(false) != 0 || !mode_is(false, 0))
		return STEP(6);
	os64_printf("ACT3\n");
	if (os64_read(0, buf, sizeof(buf)) != 0)
		return STEP(6);

	// Act 4: a grandchild goes raw and leaves it that way; the kernel
	// cooks the seat at its death. It is our foreground while we wait,
	// which is what entitles it to ask.
	int64_t kid = os64_spawn("/tests/rawtty", (char *[]){ "/tests/rawtty", "grandchild", 0 });
	if (kid < 0)
		return STEP(7);
	int32_t code = -1;
	if (os64_wait(kid, &code) != kid || code != 0)
		return STEP(7);
	if (!mode_is(false, 0))
		return STEP(7);
	os64_printf("ACT4\n");
	return 0;
}

static int grandchild(void)
{
	if (os64_tty_set_raw(true) != 0 || !mode_is(true, os64_taskid()))
		return 1;
	return 0;   // raw, and gone
}

// ── The parent: holds the master, types, watches ────────────────────────────

static bool grid_contains(int64_t master, const char *word)
{
	if (os64_pty_snapshot(master, &gHdr, gCells, COLS * ROWS) < 0)
		return false;
	size_t n = os64_strlen(word);
	for (uint32_t r = 0; r < gHdr.rows; r++)
	{
		const os64_pty_cell_t *row = &gCells[r * gHdr.cols];
		for (uint32_t c = 0; c + n <= gHdr.cols; c++)
		{
			size_t i = 0;
			while (i < n && row[c + i].ch == word[i])
				i++;
			if (i == n)
				return true;
		}
	}
	return false;
}

static bool wait_for(int64_t master, const char *word, uint32_t max_ms)
{
	for (uint32_t waited = 0; waited < max_ms; waited += 100)
	{
		if (grid_contains(master, word))
			return true;
		os64_sleep(100);
	}
	return false;
}

static bool type(int64_t master, const char *bytes, size_t n)
{
	return os64_write((int32_t)master, bytes, n) == (int64_t)n;
}

int main(int argc, char **argv)
{
	if (argc > 1 && argv[1][0] == 'c')
		os64_exit(child());
	if (argc > 1 && argv[1][0] == 'g')
		os64_exit(grandchild());

	int64_t master = os64_pty_create(COLS, ROWS);
	if (master < 0)
		os64_exit(STEP(1));
	int64_t kid = os64_spawn_seated("/tests/rawtty",
	                                (char *[]){ "/tests/rawtty", "child", 0 }, master);
	if (kid < 0)
		os64_exit(STEP(1));

	// Act 5 from this side, while the child runs: our tty is the VT, not
	// the slave, so the child's tty file must refuse our write at the open.
	char path[64];
	os64_snprintf(path, sizeof(path), "/proc/%lu/tty", (unsigned long)kid);
	int64_t h = os64_open(path, "w");
	if (h >= 0)
	{
		os64_close((int32_t)h);
		say("rawtty: FAIL a stranger's write to the child's tty file was accepted");
		os64_exit(STEP(8));
	}

	// The script the child narrates: type only once each act is on screen.
	if (!wait_for(master, "ACT1", 5000) || !type(master, "\x04", 1))
		os64_exit(STEP(3));
	if (!wait_for(master, "ACT2", 5000) || !type(master, "\x03\x04x", 3))
		os64_exit(STEP(5));
	if (!wait_for(master, "ACT3", 5000) || !type(master, "\x04", 1))
		os64_exit(STEP(6));
	if (!wait_for(master, "ACT4", 10000))
		os64_exit(STEP(7));

	int32_t code = -1;
	if (os64_wait(kid, &code) != kid)
		os64_exit(STEP(9));
	if (code != 0)
		os64_exit(code);   // the child's own step code, relayed

	os64_close((int32_t)master);
	os64_printf("rawtty: cooked by default, raw on request (0x03 0x04 arrive as bytes), cooked again, reset at the holder's exit, strangers refused\n");
	os64_exit(RAWTTY_OK);
}
