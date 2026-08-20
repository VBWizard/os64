// ptyprobe — the pty's fixture (PTY.md's stated acceptance, gfxprobe's
// tradition): the whole round trip with NO GUI anywhere. Create a pty, seat
// a real husk on the slave, type at the master, and read the interpreted
// screen back out — from a text VT, headless-testable in the harness.
//
// Three acts, each a design guarantee:
//   1. "ps"      — seating works end to end: the child's console I/O routed
//                  to the slave with zero pty-awareness, the kernel's ONE
//                  interpreter filled the grid, the snapshot read it back.
//   2. Ctrl+C    — 0x03 written at the master runs the SLAVE's intercept and
//                  kills the slave's foreground (a hog), never the probe:
//                  the per-tty aim, working for a terminal that is a task.
//   3. "exit"    — the seat count empties, HUNGUP arms, lifetime rules hold.
//
// Phase E's terminal is this program wearing libui: grid -> window blit,
// keys -> master. If ptyprobe is green, the terminal is a rendering problem.

#include "os64/os64.h"
#include "os64/pty.h"
#include "os64/proc.h"
#include "os64/fmt.h"
#include "os64/io.h"

#define COLS 80u
#define ROWS 25u

static os64_pty_header_t gHdr;
static os64_pty_cell_t   gCells[COLS * ROWS];

static int gPass = 0, gFail = 0;

static void verdict(bool ok, const char *what)
{
	os64_printf("ptyprobe: %s: %s\n", ok ? "PASS" : "FAIL", what);
	if (ok) gPass++; else gFail++;
}

static int64_t snap(int64_t master, bool cells)
{
	return os64_pty_snapshot(master, &gHdr, cells ? gCells : (os64_pty_cell_t *)0,
	                         cells ? (COLS * ROWS) : 0);
}

// Wait until the grid stops moving: generation unchanged across one interval
// after having CHANGED since `since`. Bounded — a wedge returns false.
static bool settle(int64_t master, uint64_t since, uint32_t max_ms)
{
	uint64_t last = since;
	bool moved = false;
	for (uint32_t waited = 0; waited < max_ms; waited += 100)
	{
		os64_sleep(100);
		if (snap(master, false) < 0)
			return false;
		if (gHdr.generation != last)
		{
			moved = true;
			last = gHdr.generation;
			continue;                       // still talking — keep listening
		}
		if (moved)
			return true;                    // talked, then went quiet: settled
	}
	return moved;
}

static bool grid_contains(const char *needle)
{
	size_t nlen = 0;
	while (needle[nlen]) nlen++;
	for (uint32_t r = 0; r < gHdr.rows; r++)
	{
		const os64_pty_cell_t *row = &gCells[r * gHdr.cols];
		for (uint32_t c = 0; c + nlen <= gHdr.cols; c++)
		{
			size_t i = 0;
			while (i < nlen && row[c + i].ch == needle[i]) i++;
			if (i == nlen)
				return true;
		}
	}
	return false;
}

// The screen, on OUR terminal — a text screenshot of the inner one. Rows are
// printed trimmed; fully blank trailing rows are skipped. The frame makes the
// nesting visible: a terminal inside a terminal, which is the whole trick.
static void print_grid(void)
{
	os64_printf("  +--[ pty %ux%u, cursor %u,%u, gen %lu ]--\n",
	            gHdr.cols, gHdr.rows, gHdr.cur_row, gHdr.cur_col,
	            (unsigned long)gHdr.generation);
	uint32_t lastRow = 0;
	for (uint32_t r = 0; r < gHdr.rows; r++)
		for (uint32_t c = 0; c < gHdr.cols; c++)
			if (gCells[r * gHdr.cols + c].ch > ' ')
				lastRow = r;
	for (uint32_t r = 0; r <= lastRow; r++)
	{
		char line[COLS + 1];
		uint32_t end = 0;
		for (uint32_t c = 0; c < gHdr.cols; c++)
		{
			char ch = gCells[r * gHdr.cols + c].ch;
			line[c] = (ch >= ' ') ? ch : ' ';
			if (ch > ' ') end = c + 1;
		}
		line[end] = '\0';
		os64_printf("  | %s\n", line);
	}
}

static bool type_line(int64_t master, const char *s)
{
	size_t len = 0;
	while (s[len]) len++;
	return os64_write((int32_t)master, s, len) == (int64_t)len;
}

int main(int argc, char **argv)
{
	(void)argc; (void)argv;

	int64_t master = os64_pty_create(COLS, ROWS);
	verdict(master >= 0, "pty_create returns a master handle");
	if (master < 0)
		return 1;

	// Header-only probe on a virgin pty: sane geometry, no HUNGUP (a slave
	// nothing has sat on is young, not hung up — PTY.md's everSeated rule).
	int64_t rc = snap(master, false);
	verdict(rc == 0 && gHdr.cols == COLS && gHdr.rows == ROWS,
	        "header probe: geometry as created, zero cells copied");
	verdict((gHdr.flags & OS64_PTY_HUNGUP) == 0,
	        "virgin slave is not HUNGUP (young, not abandoned)");
	uint64_t gen0 = gHdr.generation;

	// Act 1 — seat a real shell and have a conversation.
	int64_t shell = os64_spawn_seated("/bin/husk", (char *[]){ "/bin/husk", 0 },
	                                  master);
	verdict(shell > 0, "husk seated on the slave (spawn_seated)");
	if (shell <= 0)
		return 1;

	verdict(settle(master, gen0, 5000), "husk's banner+prompt reached the grid");
	rc = snap(master, true);
	verdict(rc == (int64_t)(COLS * ROWS), "full snapshot copies the live screen");
	verdict(grid_contains("husk"), "the grid shows a husk prompt");

	uint64_t genPrompt = gHdr.generation;
	verdict(type_line(master, "ps\n"), "keystrokes accepted at the master");
	verdict(settle(master, genPrompt, 5000), "ps output reached the grid");
	snap(master, true);
	verdict(grid_contains("COMMAND"), "ps rendered by the kernel's interpreter");
	print_grid();

	// Act 2 — Ctrl+C aims at the SLAVE's foreground, not at us.
	uint64_t genPs = gHdr.generation;
	verdict(type_line(master, "hog\n"), "a hog seated as the slave's foreground");
	os64_sleep(700);   // let husk spawn it and set the slave's fg
	verdict(type_line(master, "\x03"), "0x03 written at the master");
	// The proof of death is that the SHELL answers again afterwards.
	snap(master, false);
	uint64_t genIntr = gHdr.generation;
	verdict(type_line(master, "echo alive\n"), "post-interrupt echo typed");
	verdict(settle(master, genIntr, 5000) &&
	        (snap(master, true) >= 0) && grid_contains("alive"),
	        "hog died to the slave's Ctrl+C and husk answered (we never felt it)");
	(void)genPs;

	// Act 3 — the session ends: exit empties the seats, HUNGUP arms.
	verdict(type_line(master, "exit\n"), "exit typed at the shell");
	bool hungup = false;
	for (int i = 0; i < 100 && !hungup; i++)
	{
		os64_sleep(100);
		if (snap(master, false) < 0)
			break;
		hungup = (gHdr.flags & OS64_PTY_HUNGUP) != 0;
	}
	verdict(hungup, "seats emptied: the master sees HUNGUP");

	os64_close((int32_t)master);
	os64_printf("ptyprobe: %d passed, %d failed\n", gPass, gFail);
	return gFail == 0 ? 0 : 1;
}
