// winchtest — the SIGWINCH slice's fixture (PTY.md § Resize), fully headless.
//
// Three roles in one binary. With no argument it is the TERMINAL: it creates a
// GRID pty, seats a copy of itself as the program inside, resizes the pty,
// and reads the program's reports back out of the grid. With "child" it is
// the PROGRAM: it installs a SIGWINCH handler, reports its terminal's size,
// then blocks in read() the way a shell at its prompt does — because that is
// exactly where a resize finds a real program, and the interesting half of
// the slice is that a caught signal ENDS that block (OS64_INTERRUPTED) so the
// handler can run and the program can ask what changed. With "waiter" it is
// the OTHER program a resize finds: a shell blocked in wait() on a running
// child. That park has to end for a caught signal too (Codex #32 — it did
// not, and a shell could not hear a resize until its job finished), and the
// second session proves it: the waiter reports the new size while its
// grandchild is still asleep.
//
// No compositor, no window, no glass: the whole hop-2 contract — grid
// realloc, generation bump, the raise at the seats, the wake, the handler,
// the re-read — is provable in the ordinary suite. Hop 1 (the window event)
// is one line in gterm and is checked on glass.
//
// PROVE THE TEST BEFORE TRUSTING ITS GREEN (SIGNALS.md's rd9 rule): with the
// raise deleted from syscall_pty_resize (the grid grows, nobody is told)
// this fixture must exit 0x0A1D0006 — the child never reports the new size.
// It did, on 2026-08-25, before the raise went in. And with task_wait's
// signal_park_must_end check disabled it must exit 0x0A1D000C — the waiter
// never reports. It did, on 2026-08-26, the day the act was written.
//
// Exit codes (0x0A1D = "wid", the width fixture):
//   0x0A1D0000  every act green
//   0x0A1D0001  pty_create refused
//   0x0A1D0002  the child would not seat
//   0x0A1D0003  the child never reported its initial size (procfs on a pty)
//   0x0A1D0004  pty_resize refused a legal geometry
//   0x0A1D0005  the snapshot header does not show the new geometry
//   0x0A1D0006  the child never reported the grown size — the handler did
//               not run, or the blocked read was not interrupted
//   0x0A1D0007  the child never reported the shrunk size
//   0x0A1D0008  text written before the shrink did not survive it
//   0x0A1D0009  a 1x1 grid was accepted (the fence is gone)
//   0x0A1D000A  the session did not hang up after EOT
//   0x0A1D000B  the waiter would not seat, or never said it was waiting
//   0x0A1D000C  the waiter never reported the new size — wait() was not
//               interrupted for the caught signal (the Codex #32 gap)
//   0x0A1D000D  the waiter's session did not end when its grandchild did
//   0x0A1D0010  (child) /proc/self/tty could not be read
//   0x0A1D0011  (child) the SIGWINCH handler was refused
//   0x0A1D0012  (waiter) the grandchild would not spawn

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "os64/os64.h"
#include "os64/pty.h"
#include "os64/proc.h"
#include "os64/signal.h"
#include "os64/fmt.h"
#include "os64/io.h"

#define STEP(n) (0x0A1D0000u | (uint32_t)(n))

#define COLS0 80u
#define ROWS0 25u
#define MAX_CELLS (100u * 30u)   // the largest geometry this fixture asks for

static os64_pty_header_t gHdr;
static os64_pty_cell_t   gCells[MAX_CELLS];

static void die(uint32_t step, const char *why)
{
	os64_printf("winchtest: %s\n", why);
	os64_serial_log(why);
	os64_exit(STEP(step));
}

// ── the program inside ──────────────────────────────────────────────────────

static volatile int gWinch;
static void on_winch(int signo) { (void)signo; gWinch++; }

// RE-OPEN every time: procfs renders a file at OPEN, so a handle held across
// the signal would answer the old size forever (the trap PTY.md names).
static bool terminal_size(uint32_t *cols, uint32_t *rows)
{
	char buf[512];
	int64_t h = os64_open("/proc/self/tty", "r");
	if (h < 0)
		return false;
	int64_t n = os64_read((int32_t)h, buf, sizeof(buf) - 1);
	os64_close((int32_t)h);
	if (n <= 0)
		return false;
	buf[n] = 0;

	*cols = *rows = 0;
	for (char *p = buf; *p; p++)
	{
		uint32_t *out = NULL;
		if (p[0] == 'r' && p[1] == 'o' && p[2] == 'w' && p[3] == 's' && p[4] == '\t') out = rows;
		if (p[0] == 'c' && p[1] == 'o' && p[2] == 'l' && p[3] == 's' && p[4] == '\t') out = cols;
		if (out == NULL)
			continue;
		uint32_t v = 0;
		for (char *d = p + 5; *d >= '0' && *d <= '9'; d++)
			v = v * 10 + (uint32_t)(*d - '0');
		*out = v;
	}
	return *cols != 0 && *rows != 0;
}

static int child(void)
{
	if (os64_signal_set_handler(OS64_SIGWINCH, on_winch) < 0)
		die(0x11, "child: SIGWINCH handler refused");

	uint32_t c, r;
	if (!terminal_size(&c, &r))
		die(0x10, "child: /proc/self/tty unreadable");
	os64_printf("size %ux%u\n", c, r);

	// Block like a shell at its prompt. A resize lands here: the read
	// answers OS64_INTERRUPTED after the handler has run, and the program
	// asks its terminal what it is now. EOT (a read of 0) ends the act.
	for (;;)
	{
		char k;
		int64_t n = os64_read(0, &k, 1);
		if (n == OS64_INTERRUPTED)
		{
			if (!terminal_size(&c, &r))
				die(0x10, "child: /proc/self/tty unreadable after WINCH");
			os64_printf("winch %d size %ux%u\n", gWinch, c, r);
			continue;
		}
		if (n == 0)
			return 0;
	}
}

// A shell at the other kind of block: waiting on a job. The grandchild is a
// plain sleep(1) with no handler of its own — the resize reaches it too, and
// a WINCH nobody asked about is dropped at publication, so its nap is
// undisturbed and the session ends exactly when the nap does.
#define WAITER_NAP "5"

static int waiter(void)
{
	if (os64_signal_set_handler(OS64_SIGWINCH, on_winch) < 0)
		die(0x11, "waiter: SIGWINCH handler refused");

	int64_t kid = os64_spawn("/bin/sleep", (char *[]){ "/bin/sleep", WAITER_NAP, 0 });
	if (kid <= 0)
		die(0x12, "waiter: the grandchild would not spawn");
	os64_printf("waiting\n");

	// SIGNALS.md §8: an interrupted wait says so, collects nothing, and the
	// job is still ours — so ask the terminal what changed and wait again.
	for (;;)
	{
		int32_t code = 0;
		int64_t r = os64_wait(kid, &code);
		if (r == OS64_INTERRUPTED)
		{
			uint32_t c, rr;
			if (!terminal_size(&c, &rr))
				die(0x10, "waiter: /proc/self/tty unreadable after WINCH");
			os64_printf("winch %d wait %ux%u\n", gWinch, c, rr);
			continue;
		}
		return 0;
	}
}

// ── the terminal ────────────────────────────────────────────────────────────

static int64_t snap(int64_t master, bool cells)
{
	uint32_t want = gHdr.cols * gHdr.rows;
	if (want == 0 || want > MAX_CELLS)
		want = MAX_CELLS;
	return os64_pty_snapshot(master, &gHdr, cells ? gCells : (os64_pty_cell_t *)0,
	                         cells ? want : 0);
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

// Poll the grid until `needle` appears, or `max_ms` pass. The snapshot is
// re-sized to the header every pass, so it follows a resize by itself.
static bool grid_shows(int64_t master, const char *needle, uint32_t max_ms)
{
	for (uint32_t waited = 0; waited < max_ms; waited += 100)
	{
		if (snap(master, false) < 0 || snap(master, true) < 0)
			return false;
		if (grid_contains(needle))
			return true;
		os64_sleep(100);
	}
	return false;
}

int main(int argc, char **argv)
{
	if (argc > 1 && argv[1][0] == 'c')
		return child();
	if (argc > 1 && argv[1][0] == 'w')
		return waiter();

	int64_t master = os64_pty_create(COLS0, ROWS0);
	if (master < 0)
		die(1, "pty_create refused");

	int64_t kid = os64_spawn_seated("/bin/winchtest",
	                                (char *[]){ "/bin/winchtest", "child", 0 }, master);
	if (kid <= 0)
		die(2, "the child would not seat");

	// Act 1: the program reads its size off a pty through /proc/self/tty.
	if (!grid_shows(master, "size 80x25", 5000))
		die(3, "child never reported its initial size");

	// Act 2: grow. The header must show it at once; the child must be
	// interrupted out of read(), run its handler, and report the new size.
	if (os64_pty_resize(master, 100, 30) != 0)
		die(4, "pty_resize refused 100x30");
	snap(master, false);
	if (gHdr.cols != 100 || gHdr.rows != 30)
		die(5, "snapshot header does not show 100x30");
	if (!grid_shows(master, "winch 1 size 100x30", 5000))
		die(6, "child never reported the grown size (handler or wake broken)");

	// Act 3: shrink, well below both original dimensions. The text written
	// before must survive (left edges kept, the cursor is nowhere near the
	// bottom so nothing rolls into history), and the second WINCH counts.
	if (os64_pty_resize(master, 40, 10) != 0)
		die(4, "pty_resize refused 40x10");
	if (!grid_shows(master, "winch 2 size 40x10", 5000))
		die(7, "child never reported the shrunk size");
	if (!grid_contains("size 80x25"))
		die(8, "text from before the shrink did not survive it");

	// Act 4: the fence. A 1x1 terminal is nobody's terminal.
	if (os64_pty_resize(master, 1, 1) == 0)
		die(9, "a 1x1 grid was accepted");
	snap(master, false);
	if (gHdr.cols != 40 || gHdr.rows != 10)
		die(9, "a refused resize changed the geometry");

	// Act 5: EOT ends the child's read with 0; it exits; the seats empty.
	os64_write((int32_t)master, "\x04", 1);
	bool hungup = false;
	for (int i = 0; i < 50 && !hungup; i++)
	{
		os64_sleep(100);
		if (snap(master, false) < 0)
			break;
		hungup = (gHdr.flags & OS64_PTY_HUNGUP) != 0;
	}
	if (!hungup)
		die(0xA, "the session did not hang up after EOT");

	os64_close((int32_t)master);

	// Act 6: a second session, for the other block. A shell waiting on a job
	// is where a resize finds most shells most of the time; the wait has to
	// end for the caught signal (nothing collected — the job lives on), the
	// handler runs, the shell asks its terminal, and the wait resumes. The
	// grandchild's nap sets the session's length: when it ends, so does the
	// waiter, and the seats empty.
	master = os64_pty_create(COLS0, ROWS0);
	if (master < 0)
		die(1, "pty_create refused (second session)");
	kid = os64_spawn_seated("/bin/winchtest",
	                        (char *[]){ "/bin/winchtest", "waiter", 0 }, master);
	if (kid <= 0 || !grid_shows(master, "waiting", 5000))
		die(0xB, "the waiter would not seat or never said it was waiting");
	if (os64_pty_resize(master, 90, 20) != 0)
		die(4, "pty_resize refused 90x20");
	if (!grid_shows(master, "winch 1 wait 90x20", 5000))
		die(0xC, "the waiter never reported the new size (wait() not interrupted)");
	hungup = false;
	for (int i = 0; i < 100 && !hungup; i++)
	{
		os64_sleep(100);
		if (snap(master, false) < 0)
			break;
		hungup = (gHdr.flags & OS64_PTY_HUNGUP) != 0;
	}
	if (!hungup)
		die(0xD, "the waiter's session did not end with its grandchild");
	os64_close((int32_t)master);

	os64_printf("winchtest: PASS (grid follows, seats hear SIGWINCH, blocked read and wait interrupted)\n");
	os64_exit(STEP(0));
}
