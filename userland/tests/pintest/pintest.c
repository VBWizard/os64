// pintest — a handle closed by a SIBLING THREAD under an operation that is
// still inside it (handle.c § The pin).
//
// telnetd was the first program to share handles between threads and park
// in a syscall while its task tore down, and it found two ring-0 use-after-
// frees on the way (Codex #101 rd4): a reader parked in a pipe read whose
// pipe a sibling's close freed, and a spawn whose TCP reference raced the
// same close. The cure is one rule — an operation in flight holds its own
// reference on the object — and this fixture drives that rule from ring 3,
// without a network, in the three shapes the kernel has to get right:
//
//   1. A PIPE: a thread parks reading the read end; the main thread closes
//      that end (the sibling close), then the write end (the EOF). Before
//      the pin the second close freed the pipe under the parked reader and
//      the lazy HHDM tripwire killed the MACHINE; now the reader is the
//      pipe's last holder, gets its EOF, and frees it on the way out.
//   2. A STREAM PTY MASTER — the exact shape Codex found in telnetd: a
//      thread parks reading the master; the main thread closes the master.
//      Both pipe ends close inside that one call (never seated), and the
//      reader must still come back with EOF instead of a freed pipe.
//   3. A THREAD HANDLE: a thread parks joining a worker; the main thread
//      closes the join handle; the worker finishes. The join object's two
//      old references are both gone by then, and only the reader's pin
//      keeps the answer alive long enough to be read.
//
// A pass is the ordinary answer in every case — 0, 0, and the worker's
// value. The failing version of each step does not return a wrong number;
// it panics the kernel, which is why the steps are ordered from the most to
// the least general and why each has its own exit code.
//
// Exit codes: 0x91A70000 success, and a distinct code per failed step.

#include "os64/os64.h"
#include "os64/pty.h"

#define PINTEST_OK             0x91A70000
#define PINTEST_NO_PIPE        0x91A70001
#define PINTEST_NO_THREAD      0x91A70002
#define PINTEST_PIPE_BAD_EOF   0x91A70003   // the parked pipe reader came back with something other than EOF
#define PINTEST_NO_PTY         0x91A70004
#define PINTEST_PTY_BAD_EOF    0x91A70005   // the parked master reader came back with something other than EOF
#define PINTEST_JOIN_BAD_VALUE 0x91A70006   // the parked joiner did not get the worker's answer

// Enough for the reader to be genuinely PARKED (ISLEEP) before the sibling
// closes — the race the fixture exists to drive is "asleep inside", not
// "about to enter". Two scheduler ticks would do; twenty are cheap.
#define SETTLE_MS 200

static int64_t read_one(void *arg)
{
	int32_t h = (int32_t)(int64_t)arg;
	char byte;
	return os64_read(h, &byte, 1);   // parks: nothing to read, a writer exists
}

static int64_t join_one(void *arg)
{
	int32_t h = (int32_t)(int64_t)arg;
	int64_t answer = -1;
	if (os64_thread_join(h, &answer) < 0)
		return -1;
	return answer;
}

static int64_t worker(void *arg)
{
	(void)arg;
	os64_sleep(SETTLE_MS * 2);   // outlive the joiner's park AND the sibling close
	return 4242;
}

int main(int argc, char **argv)
{
	(void)argc; (void)argv;

	// ── 1. The pipe ──────────────────────────────────────────────────────
	int32_t p[2];
	if (os64_pipe(p) < 0)
		return PINTEST_NO_PIPE;
	int64_t reader = os64_thread(read_one, (void *)(int64_t)p[0]);
	if (reader < 0)
		return PINTEST_NO_THREAD;
	os64_sleep(SETTLE_MS);
	os64_close(p[0]);   // the sibling close: the reader is still inside this end
	os64_close(p[1]);   // the last writer: EOF for whoever is still reading
	int64_t got = -1;
	os64_thread_join((int32_t)reader, &got);
	if (got != 0)
	{
		os64_printf("pintest: pipe reader answered %ld, wanted EOF (0)\n", (long)got);
		return PINTEST_PIPE_BAD_EOF;
	}

	// ── 2. The stream pty master ─────────────────────────────────────────
	int64_t master = os64_pty_create_stream(80, 24);
	if (master < 0)
		return PINTEST_NO_PTY;
	reader = os64_thread(read_one, (void *)master);
	if (reader < 0)
		return PINTEST_NO_THREAD;
	os64_sleep(SETTLE_MS);
	os64_close((int32_t)master);   // the sibling close: hangs up both pipe ends at once
	got = -1;
	os64_thread_join((int32_t)reader, &got);
	if (got != 0)
	{
		os64_printf("pintest: master reader answered %ld, wanted EOF (0)\n", (long)got);
		return PINTEST_PTY_BAD_EOF;
	}

	// ── 3. The thread handle ─────────────────────────────────────────────
	int64_t w = os64_thread(worker, NULL);
	if (w < 0)
		return PINTEST_NO_THREAD;
	int64_t joiner = os64_thread(join_one, (void *)w);
	if (joiner < 0)
		return PINTEST_NO_THREAD;
	os64_sleep(SETTLE_MS);
	os64_close((int32_t)w);   // the sibling close: the joiner is still inside this handle
	got = -1;
	os64_thread_join((int32_t)joiner, &got);
	if (got != 4242)
	{
		os64_printf("pintest: joiner answered %ld, wanted the worker's 4242\n", (long)got);
		return PINTEST_JOIN_BAD_VALUE;
	}

	os64_printf("pintest: a parked reader outlives its sibling's close: pipe, pty master, thread handle\n");
	return PINTEST_OK;
}
