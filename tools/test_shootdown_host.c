// The waited invalidation's rounds (kernel/include/shootdown.h) beside a
// model of page tables and the translations each CPU keeps.
//
// The model: the tables have a version, bumped by every change a waiter
// makes; a CPU's translations are the version it last flushed at. Waiters
// and flush handlers are state machines stepped in a random order, as CPUs
// interleave. A handler follows the kernel's order (inv_tlb_ISR): read the
// round, flush, say so. The rule checked: when a waiter's round is answered
// by every CPU, every CPU's translations are at least as new as the
// waiter's change, so none can still reach what the change removed.
// Directed cases check the counts.
//
//   test_shootdown [random runs]

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shootdown.h"

static unsigned long s_checks, s_failed;

#define CHECK(cond, ...)                                                                  \
	do {                                                                                  \
		s_checks++;                                                                       \
		if (!(cond)) {                                                                    \
			s_failed++;                                                                   \
			fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                          \
			fprintf(stderr, __VA_ARGS__);                                                 \
			fputc('\n', stderr);                                                          \
			if (s_failed > 20)                                                            \
				exit(1);                                                                  \
		}                                                                                 \
	} while (0)

#define CPUS 4
#define WAITERS 3

static shootdown_t s_sd;
static unsigned long s_tables;          // the tables' version
static unsigned long s_tlb[CPUS];       // the version each CPU flushed at
static int s_ipis[CPUS];                // flush requests not yet taken

typedef struct
{
	int cpu, step, rounds_left;
	unsigned long need;   // the tables' version after this waiter's change
	uint64_t round;
} waiter_t;

typedef struct
{
	int step;   // 0 idle, 1 read the round, 2 flushed
	uint64_t covers;
} handler_t;

static waiter_t s_w[WAITERS];
static handler_t s_h[CPUS];

// One step of waiter w. Returns false when it has no more rounds to run.
static bool waiter_step(waiter_t *w)
{
	switch (w->step)
	{
	case 0:   // change the tables
		if (w->rounds_left == 0)
			return false;
		w->need = ++s_tables;
		w->step = 1;
		return true;
	case 1:   // begin, ask the others, flush its own
		w->round = shootdown_begin(&s_sd);
		for (int c = 0; c < CPUS; c++)
			if (c != w->cpu)
				s_ipis[c]++;
		s_tlb[w->cpu] = s_tables;
		w->step = 2;
		return true;
	default:  // wait
		for (int c = 0; c < CPUS; c++)
			if (c != w->cpu && !shootdown_answered(&s_sd, (uint32_t)c, w->round))
				return true;
		for (int c = 0; c < CPUS; c++)
			CHECK(s_tlb[c] >= w->need, "round %lu answered, CPU %d flushed at %lu, before the change at %lu",
			      (unsigned long)w->round, c, s_tlb[c], w->need);
		w->rounds_left--;
		w->step = 0;
		return true;
	}
}

static void handler_step(int c)
{
	handler_t *h = &s_h[c];
	switch (h->step)
	{
	case 0:
		if (s_ipis[c] == 0)
			return;
		s_ipis[c]--;
		h->covers = shootdown_flush_covers(&s_sd);
		h->step = 1;
		return;
	case 1:
		s_tlb[c] = s_tables;
		h->step = 2;
		return;
	default:
		shootdown_flushed(&s_sd, (uint32_t)c, h->covers);
		h->step = 0;
		return;
	}
}

static void one_run(void)
{
	memset(&s_sd, 0, sizeof(s_sd));
	memset(s_tlb, 0, sizeof(s_tlb));
	memset(s_ipis, 0, sizeof(s_ipis));
	memset(s_h, 0, sizeof(s_h));
	s_tables = 0;
	for (int i = 0; i < WAITERS; i++)
		s_w[i] = (waiter_t){ .cpu = i % CPUS, .rounds_left = 1 + rand() % 4 };
	for (int steps = 0; steps < 100000; steps++)
	{
		bool any = false;
		for (int i = 0; i < WAITERS; i++)
			any |= s_w[i].rounds_left != 0;
		for (int c = 0; c < CPUS; c++)
			any |= s_ipis[c] != 0 || s_h[c].step != 0;
		if (!any)
			return;
		int pick = rand() % (WAITERS + CPUS);
		if (pick < WAITERS)
			(void)waiter_step(&s_w[pick]);
		else
			handler_step(pick - WAITERS);
	}
	CHECK(false, "a run did not finish: a round never answered");
}

static void directed(void)
{
	memset(&s_sd, 0, sizeof(s_sd));
	// Numbers rise one a round; a CPU that flushed nothing answers nothing.
	CHECK(shootdown_begin(&s_sd) == 1 && shootdown_begin(&s_sd) == 2, "rounds are not numbered 1, 2");
	CHECK(!shootdown_answered(&s_sd, 1, 1), "an unflushed CPU answered round 1");
	CHECK(shootdown_flush_covers(&s_sd) == 2, "a flush now covers %lu, not 2",
	      (unsigned long)shootdown_flush_covers(&s_sd));
	// A flush for round 2 answers rounds 1 and 2 and not 3.
	shootdown_flushed(&s_sd, 1, 2);
	CHECK(shootdown_answered(&s_sd, 1, 1) && shootdown_answered(&s_sd, 1, 2), "a flush for 2 left 1 or 2 unanswered");
	CHECK(!shootdown_answered(&s_sd, 1, 3), "a flush for 2 answered round 3");
	CHECK(!shootdown_answered(&s_sd, 2, 1), "CPU 1's flush answered for CPU 2");
	shootdown_flushed(&s_sd, 2, 1);
	CHECK(shootdown_answered(&s_sd, 2, 1) && !shootdown_answered(&s_sd, 2, 2), "CPU 2's flush for 1 misjudged");
	CHECK(shootdown_answered(&s_sd, SHOOTDOWN_MAX_CPUS, 9), "a CPU past the table is waited for");

	// The books: rounds, the late ones, the longest wait, and the window.
	CHECK(!shootdown_settled(&s_sd, 40, (shootdown_last_t){ 1, 100, 1000 }, 100), "40 us of 100 called late");
	CHECK(!shootdown_settled(&s_sd, 100, (shootdown_last_t){ 2, 200, 2000 }, 100), "exactly the limit called late");
	CHECK(shootdown_settled(&s_sd, 250, (shootdown_last_t){ 3, 300, 3000 }, 100), "250 us of 100 not called late");
	CHECK(!shootdown_settled(&s_sd, 70, (shootdown_last_t){ 4, 400, 4000 }, 100), "70 us of 100 called late");
	shootdown_counts_t c;
	shootdown_read(&s_sd, &c);
	CHECK(c.rounds == 4 && c.late == 1 && c.wait_most_us == 250, "totals %lu rounds %lu late %lu most",
	      (unsigned long)c.rounds, (unsigned long)c.late, (unsigned long)c.wait_most_us);
	CHECK(c.wait_most_cpu == 3 && c.window_wait_most_cpu == 3, "the longest wait's CPU %u/%u, not 3",
	      c.wait_most_cpu, c.window_wait_most_cpu);
	CHECK(c.wait_most_thread == 300 && c.wait_most_task == 3000 && c.window_wait_most_thread == 300 &&
	          c.window_wait_most_task == 3000,
	      "the longest wait's thread %lu/%lu and task %lu/%lu, not 300 and 3000",
	      (unsigned long)c.wait_most_thread, (unsigned long)c.window_wait_most_thread,
	      (unsigned long)c.wait_most_task, (unsigned long)c.window_wait_most_task);
	CHECK(c.window_rounds == 4 && c.window_late == 1 && c.window_wait_most_us == 250,
	      "window %lu rounds %lu late %lu most", (unsigned long)c.window_rounds, (unsigned long)c.window_late,
	      (unsigned long)c.window_wait_most_us);
	CHECK(!shootdown_settled(&s_sd, 30, (shootdown_last_t){ 5, 500, 5000 }, 100), "30 us called late");
	shootdown_read(&s_sd, &c);
	CHECK(c.wait_most_cpu == 3 && c.window_wait_most_cpu == 5, "after a window: CPUs %u/%u, not 3 and 5",
	      c.wait_most_cpu, c.window_wait_most_cpu);
	CHECK(c.wait_most_thread == 300 && c.window_wait_most_thread == 500 && c.wait_most_task == 3000 &&
	          c.window_wait_most_task == 5000,
	      "after a window: threads %lu/%lu tasks %lu/%lu", (unsigned long)c.wait_most_thread,
	      (unsigned long)c.window_wait_most_thread, (unsigned long)c.wait_most_task,
	      (unsigned long)c.window_wait_most_task);
	CHECK(c.rounds == 5 && c.late == 1 && c.wait_most_us == 250, "totals moved wrongly after a window");
	CHECK(c.window_rounds == 1 && c.window_late == 0 && c.window_wait_most_us == 30,
	      "second window %lu rounds %lu late %lu most", (unsigned long)c.window_rounds,
	      (unsigned long)c.window_late, (unsigned long)c.window_wait_most_us);
	shootdown_read(&s_sd, &c);
	CHECK(c.window_rounds == 0 && c.window_late == 0 && c.window_wait_most_us == 0, "an empty window is not empty");
}

int main(int argc, char **argv)
{
	unsigned long runs = argc > 1 ? strtoul(argv[1], NULL, 10) : 20000;
	directed();
	srand(11);
	for (unsigned long r = 0; r < runs; r++)
		one_run();
	printf("shootdown rounds: %lu checks, %lu failed\n", s_checks, s_failed);
	return s_failed != 0;
}
