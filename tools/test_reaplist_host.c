// The reap list's lifecycle (kernel/include/reaplist.h) beside a model:
// directed cases, then random operations on a few dozen nodes of a few
// owners, every answer and the whole list checked after each step.
//
//   test_reaplist [random rounds]

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "reaplist.h"

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

#define NODES 40
#define OWNERS 4

typedef struct
{
	reap_state_t state[NODES];
	int owner_of[NODES];
	int queue[NODES], queued;   // node indices, oldest first
	unsigned pending[OWNERS];
	unsigned queued_most, window_queued_most;
	unsigned long handed, claimed_next, claimed_named, finished, refused, window_handed, window_finished;
} model_t;

static reaplist_t s_list;
static reap_node_t s_nodes[NODES];
static reap_owner_t s_owners[OWNERS];
static model_t s_model;

static void reset(void)
{
	memset(&s_list, 0, sizeof(s_list));
	memset(s_nodes, 0, sizeof(s_nodes));
	memset(s_owners, 0, sizeof(s_owners));
	memset(&s_model, 0, sizeof(s_model));
}

static int index_of(reap_node_t *n)
{
	return n == NULL ? -1 : (int)(n - s_nodes);
}

// The list's links against the model's queue, and every node's state.
static void check_all(const char *after)
{
	int i = 0;
	reap_node_t *last = NULL;
	for (reap_node_t *n = s_list.head; n != NULL && i <= NODES; n = n->next, i++)
	{
		CHECK(i < s_model.queued && index_of(n) == s_model.queue[i], "%s: list position %d holds node %d", after, i,
		      index_of(n));
		CHECK(n->state == REAP_PENDING, "%s: node %d on the list in state %d", after, index_of(n), n->state);
		last = n;
	}
	CHECK(i == s_model.queued, "%s: list walks %d nodes, model queues %d", after, i, s_model.queued);
	CHECK(s_list.tail == last, "%s: tail is node %d, last walked %d", after, index_of(s_list.tail), index_of(last));
	CHECK(s_list.queued == (unsigned)s_model.queued, "%s: queued %u, model %d", after, s_list.queued, s_model.queued);
	for (int k = 0; k < NODES; k++)
		CHECK(s_nodes[k].state == s_model.state[k], "%s: node %d state %d, model %d", after, k, s_nodes[k].state,
		      s_model.state[k]);
	for (int o = 0; o < OWNERS; o++)
		CHECK(s_owners[o].pending == s_model.pending[o], "%s: owner %d pending %u, model %u", after, o,
		      s_owners[o].pending, s_model.pending[o]);
	CHECK(s_list.handed == s_model.handed && s_list.claimed_next == s_model.claimed_next &&
	          s_list.claimed_named == s_model.claimed_named && s_list.finished == s_model.finished &&
	          s_list.refused == s_model.refused,
	      "%s: counts handed %lu/%lu next %lu/%lu named %lu/%lu finished %lu/%lu refused %lu/%lu", after,
	      (unsigned long)s_list.handed, s_model.handed, (unsigned long)s_list.claimed_next, s_model.claimed_next,
	      (unsigned long)s_list.claimed_named, s_model.claimed_named, (unsigned long)s_list.finished,
	      s_model.finished, (unsigned long)s_list.refused, s_model.refused);
	CHECK(s_list.queued_most == s_model.queued_most && s_list.window_queued_most == s_model.window_queued_most,
	      "%s: most %u/%u window most %u/%u", after, s_list.queued_most, s_model.queued_most,
	      s_list.window_queued_most, s_model.window_queued_most);
}

static void model_dequeue(int pos)
{
	memmove(&s_model.queue[pos], &s_model.queue[pos + 1], (size_t)(s_model.queued - pos - 1) * sizeof(int));
	s_model.queued--;
}

static void op_hand(int k, int o)
{
	bool want = s_model.state[k] == REAP_NONE;
	bool got = reaplist_hand(&s_list, &s_nodes[k], &s_owners[o]);
	CHECK(got == want, "hand node %d (state %d): answered %d", k, s_model.state[k], got);
	if (!want)
	{
		s_model.refused++;
		return;
	}
	s_model.state[k] = REAP_PENDING;
	s_model.owner_of[k] = o;
	s_model.queue[s_model.queued++] = k;
	s_model.pending[o]++;
	s_model.handed++;
	s_model.window_handed++;
	if ((unsigned)s_model.queued > s_model.queued_most)
		s_model.queued_most = (unsigned)s_model.queued;
	if ((unsigned)s_model.queued > s_model.window_queued_most)
		s_model.window_queued_most = (unsigned)s_model.queued;
	CHECK(s_nodes[k].owner == &s_owners[o], "hand node %d: owner not recorded", k);
}

static void op_claim_next(void)
{
	int want = s_model.queued ? s_model.queue[0] : -1;
	reap_node_t *got = reaplist_claim_next(&s_list);
	CHECK(index_of(got) == want, "claim_next: node %d, model %d", index_of(got), want);
	if (want < 0)
		return;
	model_dequeue(0);
	s_model.state[want] = REAP_RUNNING;
	s_model.claimed_next++;
	CHECK(got == NULL || got->next == NULL, "claim_next: node %d keeps a link", want);
}

static void op_claim(int k)
{
	int pos = -1;
	for (int i = 0; i < s_model.queued; i++)
		if (s_model.queue[i] == k)
			pos = i;
	bool want = pos >= 0;
	bool got = reaplist_claim(&s_list, &s_nodes[k]);
	CHECK(got == want, "claim node %d (state %d): answered %d", k, s_model.state[k], got);
	if (!want)
	{
		s_model.refused++;
		return;
	}
	model_dequeue(pos);
	s_model.state[k] = REAP_RUNNING;
	s_model.claimed_named++;
}

static void op_finish(int k)
{
	bool want = s_model.state[k] == REAP_RUNNING;
	bool got = reaplist_finish(&s_list, &s_nodes[k]);
	CHECK(got == want, "finish node %d (state %d): answered %d", k, s_model.state[k], got);
	if (!want)
	{
		s_model.refused++;
		return;
	}
	s_model.state[k] = REAP_DONE;
	s_model.pending[s_model.owner_of[k]]--;
	s_model.finished++;
	s_model.window_finished++;
}

static void op_read(void)
{
	reaplist_counts_t c;
	reaplist_read(&s_list, &c);
	CHECK(c.queued == (unsigned)s_model.queued && c.queued_most == s_model.queued_most &&
	          c.window_queued_most == s_model.window_queued_most && c.handed == s_model.handed &&
	          c.claimed_next == s_model.claimed_next && c.claimed_named == s_model.claimed_named &&
	          c.finished == s_model.finished && c.refused == s_model.refused &&
	          c.window_handed == s_model.window_handed && c.window_finished == s_model.window_finished,
	      "read: the counts do not match the model");
	s_model.window_queued_most = (unsigned)s_model.queued;
	s_model.window_handed = 0;
	s_model.window_finished = 0;
}

static void directed(void)
{
	reset();
	// Empty: nothing to claim, nothing to finish.
	CHECK(reaplist_claim_next(&s_list) == NULL, "empty list gave a node");
	op_finish(0);
	op_claim(0);
	check_all("empty");

	// Three handed, claimed oldest first; a named claim from the middle.
	op_hand(0, 0);
	op_hand(1, 1);
	op_hand(2, 0);
	check_all("three handed");
	op_hand(1, 1);   // twice: refused, still counted once
	op_claim(1);     // the middle
	check_all("middle claimed");
	op_claim(1);     // RUNNING: refused
	op_finish(1);
	op_finish(1);    // DONE: refused
	op_hand(1, 1);   // DONE never returns to the list
	check_all("middle done");
	op_claim_next(); // node 0
	op_claim(2);     // the tail, now the only node
	check_all("emptied");
	op_hand(3, 2);   // a fresh tail after the list emptied
	check_all("handed after emptying");
	op_read();
	op_read();       // a second read sees an empty window
	op_finish(0);
	op_finish(2);
	op_claim_next();
	op_finish(3);
	check_all("all done");
	for (int o = 0; o < OWNERS; o++)
		CHECK(s_owners[o].pending == 0, "owner %d still pending %u", o, s_owners[o].pending);
}

int main(int argc, char **argv)
{
	unsigned long rounds = argc > 1 ? strtoul(argv[1], NULL, 10) : 20000;
	directed();

	srand(7);
	for (unsigned long r = 0; r < rounds; r++)
	{
		if (r % 400 == 0)
			reset();
		int k = rand() % NODES;
		switch (rand() % 9)
		{
		case 0: case 1: case 2:
			op_hand(k, rand() % OWNERS);
			break;
		case 3: case 4:
			op_claim_next();
			break;
		case 5:
			op_claim(k);
			break;
		case 6: case 7:
			op_finish(k);
			break;
		default:
			op_read();
			break;
		}
		check_all("random");
	}
	printf("reap list: %lu checks, %lu failed\n", s_checks, s_failed);
	return s_failed != 0;
}
