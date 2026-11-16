// test_media_keys_host.c — kernel/src/audio/media_keys.c on the host, beside
// a plain model of MEDIA_KEYS.md § The rule. Directed cases name each
// sentence of the rule; random histories hold the code to the model.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio/media_keys.h"

static int s_failed, s_checks;

#define CHECK(cond, ...) do { s_checks++; if (!(cond)) { s_failed++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ---------------------------------------------------------------- directed

static media_keys_table_t T;
static media_keys_session_t S[MEDIA_KEYS_SESSIONS + 1];

static media_keys_session_t *open_with(int i, const uint64_t *lineage, uint32_t n)
{
	media_keys_session_init(&S[i], lineage, n);
	CHECK(media_keys_add(&T, &S[i]) >= 0, "add %d", i);
	return &S[i];
}

static void drain(media_keys_session_t *s)
{
	os64_media_key_t k;
	while (media_keys_take(s, &k)) {}
}

static void directed(void)
{
	media_keys_table_init(&T);
	media_keys_route_t r;

	// No session: Play and Pause are the kernel's; the others go unanswered.
	r = media_keys_press(&T, OS64_MEDIA_KEY_PLAY, 0, 1);
	CHECK(r.target == NULL && r.kernel_toggle && T.last_rule == MEDIA_KEYS_RULE_KERNEL, "kernel play");
	r = media_keys_press(&T, OS64_MEDIA_KEY_NEXT, 0, 2);
	CHECK(r.target == NULL && !r.kernel_toggle && T.unanswered == 1, "next unanswered");
	media_keys_kernel_failed(&T);
	CHECK(T.unanswered == 2, "kernel failed counts");

	// Rule 1: a playing session takes it, and the newest to START playing wins.
	uint64_t a_line[] = {10, 5, 1}, b_line[] = {20, 5, 1}, c_line[] = {30, 7};
	media_keys_session_t *a = open_with(0, a_line, 3), *b = open_with(1, b_line, 3), *c = open_with(2, c_line, 2);
	media_keys_set_state(&T, a, MEDIA_KEYS_PLAYING);
	media_keys_set_state(&T, b, MEDIA_KEYS_PLAYING);
	r = media_keys_press(&T, OS64_MEDIA_KEY_NEXT, 30, 3);
	CHECK(r.target == b && T.last_rule == MEDIA_KEYS_RULE_PLAYING, "newest playing wins over focus");
	media_keys_set_state(&T, a, MEDIA_KEYS_PLAYING);   // saying it again is not starting again
	r = media_keys_press(&T, OS64_MEDIA_KEY_NEXT, 0, 4);
	CHECK(r.target == b, "repeating playing does not move the keys");
	media_keys_set_state(&T, a, MEDIA_KEYS_PAUSED);
	media_keys_set_state(&T, a, MEDIA_KEYS_PLAYING);
	r = media_keys_press(&T, OS64_MEDIA_KEY_NEXT, 0, 5);
	CHECK(r.target == a, "starting again moves them");

	// The toggle: Play to a playing session is given as Pause, and Pause to a paused one as Play.
	r = media_keys_press(&T, OS64_MEDIA_KEY_PLAY, 0, 6);
	CHECK(r.target == a && r.delivered == OS64_MEDIA_KEY_PAUSE, "play to playing is pause");
	drain(a); drain(b);
	media_keys_set_state(&T, a, MEDIA_KEYS_PAUSED);
	media_keys_set_state(&T, b, MEDIA_KEYS_PAUSED);
	r = media_keys_press(&T, OS64_MEDIA_KEY_PAUSE, 0, 7);
	CHECK(r.delivered == OS64_MEDIA_KEY_PLAY, "pause to paused is play");
	r = media_keys_press(&T, OS64_MEDIA_KEY_STOP, 0, 8);
	CHECK(r.delivered == OS64_MEDIA_KEY_STOP, "stop goes through");
	drain(a); drain(b); drain(c);

	// Rule 2: nobody playing; the focused window's owner in a lineage, the nearest.
	r = media_keys_press(&T, OS64_MEDIA_KEY_NEXT, 30, 9);
	CHECK(r.target == c && T.last_rule == MEDIA_KEYS_RULE_FOCUS, "focus at place 0");
	r = media_keys_press(&T, OS64_MEDIA_KEY_NEXT, 7, 10);
	CHECK(r.target == c, "focus on an ancestor");
	// Two equally near (5 is both a's and b's parent): the most recently active.
	r = media_keys_press(&T, OS64_MEDIA_KEY_NEXT, 5, 11);
	media_keys_session_t *first = r.target;
	CHECK(first == a || first == b, "an equal tie picks one of them");
	media_keys_session_t *other = first == a ? b : a;
	media_keys_set_state(&T, other, MEDIA_KEYS_PAUSED);
	r = media_keys_press(&T, OS64_MEDIA_KEY_NEXT, 5, 12);
	CHECK(r.target == other, "among the equally near, the most recently active");
	// Nearer beats more recent.
	media_keys_set_state(&T, b, MEDIA_KEYS_STOPPED);
	r = media_keys_press(&T, OS64_MEDIA_KEY_NEXT, 10, 13);
	CHECK(r.target == a, "nearer beats more recent");
	drain(a); drain(b); drain(c);

	// Rule 3: no focus match, the most recently active.
	media_keys_set_state(&T, c, MEDIA_KEYS_STOPPED);
	r = media_keys_press(&T, OS64_MEDIA_KEY_NEXT, 999, 14);
	CHECK(r.target == c && T.last_rule == MEDIA_KEYS_RULE_ACTIVE, "most recently active");
	r = media_keys_press(&T, OS64_MEDIA_KEY_NEXT, 0, 15);
	CHECK(r.target == c && T.last_rule == MEDIA_KEYS_RULE_ACTIVE, "no focus at all is rule 3");
	drain(a); drain(b); drain(c);

	// A full ring is passed over at every rule, down to the kernel.
	media_keys_set_state(&T, a, MEDIA_KEYS_PLAYING);
	for (uint32_t i = 0; i < MEDIA_KEYS_RING; i++)
		r = media_keys_press(&T, OS64_MEDIA_KEY_NEXT, 0, 16 + i);
	CHECK(a->count == MEDIA_KEYS_RING, "a's ring full");
	r = media_keys_press(&T, OS64_MEDIA_KEY_NEXT, 0, 30);
	CHECK(r.target != a && a->passed_full && r.target != NULL, "full playing session passed over");
	media_keys_remove(&T, b);
	media_keys_remove(&T, c);
	r = media_keys_press(&T, OS64_MEDIA_KEY_PLAY, 0, 31);
	CHECK(r.target == NULL && r.kernel_toggle, "every ring full: the kernel's");
	os64_media_key_t k;
	CHECK(media_keys_take(a, &k) && k.serial == a->serial - MEDIA_KEYS_RING + 1 && k.at_us == 16, "oldest first, stamped");
	r = media_keys_press(&T, OS64_MEDIA_KEY_NEXT, 0, 32);
	CHECK(r.target == a && !a->passed_full, "room again: taken again");

	// Removal strikes it.
	media_keys_remove(&T, a);
	r = media_keys_press(&T, OS64_MEDIA_KEY_NEXT, 0, 33);
	CHECK(r.target == NULL && T.listed == 0, "removed sessions are never reached");

	// Undelivered counts lost.
	media_keys_session_t *d = open_with(3, a_line, 1);
	r = media_keys_press(&T, OS64_MEDIA_KEY_NEXT, 0, 34);
	CHECK(media_keys_take(d, &k), "take");
	media_keys_undelivered(d);
	CHECK(d->lost == 1 && d->delivered == 0, "undelivered is lost");
	media_keys_remove(&T, d);

	// The wall.
	media_keys_table_init(&T);
	for (uint32_t i = 0; i < MEDIA_KEYS_SESSIONS; i++)
		open_with((int)i, a_line, 1);
	media_keys_session_init(&S[MEDIA_KEYS_SESSIONS], a_line, 1);
	CHECK(media_keys_add(&T, &S[MEDIA_KEYS_SESSIONS]) == -1, "past the wall refused");

	// Lineage beyond the cap keeps the nearest.
	uint64_t long_line[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
	media_keys_session_init(&S[0], long_line, 12);
	CHECK(S[0].lineage_len == MEDIA_KEYS_LINEAGE && S[0].lineage[7] == 8, "nearest eight kept");

	// The words.
	uint8_t st = 99;
	CHECK(media_keys_parse_state("playing", 7, &st) && st == MEDIA_KEYS_PLAYING, "playing");
	CHECK(media_keys_parse_state("paused\n", 7, &st) && st == MEDIA_KEYS_PAUSED, "trailing newline");
	CHECK(media_keys_parse_state("stopped \t", 9, &st) && st == MEDIA_KEYS_STOPPED, "trailing blanks");
	st = 99;
	CHECK(!media_keys_parse_state(" playing", 8, &st) && st == 99, "leading blank refused, state untouched");
	CHECK(!media_keys_parse_state("play", 4, &st), "a prefix refused");
	CHECK(!media_keys_parse_state("playingx", 8, &st), "a longer word refused");
	CHECK(!media_keys_parse_state("Playing", 7, &st), "case matters");
	CHECK(!media_keys_parse_state("", 0, &st), "empty refused");
}

// ---------------------------------------------------------------- the model

typedef struct {
	bool open;
	uint8_t state;
	uint64_t lineage[MEDIA_KEYS_LINEAGE];
	uint32_t n;
	uint64_t started, active;   // the model's own clock
	uint32_t queued;
} model_t;

static model_t M[MEDIA_KEYS_SESSIONS];
static uint64_t s_clock;

// The rule, written from the design's words with no shared code.
static int model_pick(uint64_t focus, uint32_t *rule)
{
	int best = -1;
	for (int i = 0; i < (int)MEDIA_KEYS_SESSIONS; i++)
		if (M[i].open && M[i].queued < MEDIA_KEYS_RING && M[i].state == MEDIA_KEYS_PLAYING &&
		    (best < 0 || M[i].started > M[best].started))
			best = i;
	if (best >= 0) { *rule = 1; return best; }
	int bestnear = 1000;
	if (focus != 0)
		for (int i = 0; i < (int)MEDIA_KEYS_SESSIONS; i++) {
			if (!M[i].open || M[i].queued >= MEDIA_KEYS_RING) continue;
			for (uint32_t j = 0; j < M[i].n; j++)
				if (M[i].lineage[j] == focus) {
					if ((int)j < bestnear || ((int)j == bestnear && M[i].active > M[best].active)) {
						best = i;
						bestnear = (int)j;
					}
					break;
				}
		}
	if (best >= 0) { *rule = 2; return best; }
	for (int i = 0; i < (int)MEDIA_KEYS_SESSIONS; i++)
		if (M[i].open && M[i].queued < MEDIA_KEYS_RING && (best < 0 || M[i].active > M[best].active))
			best = i;
	*rule = best >= 0 ? 3 : 4;
	return best;
}

static uint32_t rnd(uint32_t n) { return (uint32_t)rand() % n; }

static void random_history(int rounds)
{
	for (int round = 0; round < rounds; round++) {
		media_keys_table_init(&T);
		memset(M, 0, sizeof(M));
		s_clock = 0;
		for (int step = 0; step < 400; step++) {
			int i = (int)rnd(8);   // a few sessions, so they meet often
			switch (rnd(6)) {
			case 0:
				if (!M[i].open) {
					uint64_t line[MEDIA_KEYS_LINEAGE];
					uint32_t n = 1 + rnd(4);
					for (uint32_t j = 0; j < n; j++)
						line[j] = 1 + rnd(12);   // shared ancestors, ties likely
					media_keys_session_init(&S[i], line, n);
					CHECK(media_keys_add(&T, &S[i]) >= 0, "add");
					M[i] = (model_t){.open = true, .n = n, .active = ++s_clock};
					memcpy(M[i].lineage, line, sizeof(line[0]) * n);
				} else {
					media_keys_remove(&T, &S[i]);
					M[i].open = false;
				}
				break;
			case 1: case 2:
				if (M[i].open) {
					uint8_t st = (uint8_t)rnd(3);
					media_keys_set_state(&T, &S[i], st);
					s_clock++;
					if (st == MEDIA_KEYS_PLAYING && M[i].state != MEDIA_KEYS_PLAYING)
						M[i].started = s_clock;
					M[i].state = st;
					M[i].active = s_clock;
				}
				break;
			case 3:
				if (M[i].open) {
					os64_media_key_t k;
					bool got = media_keys_take(&S[i], &k);
					CHECK(got == (M[i].queued > 0), "take agrees with the model");
					if (got) M[i].queued--;
				}
				break;
			default: {
				uint32_t key = 1 + rnd(5);
				uint64_t focus = rnd(3) ? 1 + rnd(12) : 0;
				uint32_t want_rule;
				int want = model_pick(focus, &want_rule);
				media_keys_route_t r = media_keys_press(&T, key, focus, (uint64_t)step);
				int got = r.target ? (int)(r.target - S) : -1;
				CHECK(got == want && T.last_rule == want_rule, "round %d step %d: picked %d by %u, model %d by %u",
				      round, step, got, T.last_rule, want, want_rule);
				bool toggle = key == OS64_MEDIA_KEY_PLAY || key == OS64_MEDIA_KEY_PAUSE;
				if (want >= 0) {
					uint32_t given = toggle ? (M[want].state == MEDIA_KEYS_PLAYING ? OS64_MEDIA_KEY_PAUSE
					                                                                : OS64_MEDIA_KEY_PLAY) : key;
					CHECK(r.delivered == given, "delivered %u, model %u", r.delivered, given);
					M[want].queued++;
					s_clock++;
					M[want].active = s_clock;
				} else {
					CHECK(r.kernel_toggle == toggle, "kernel toggle only for play and pause");
				}
				break;
			}
			}
		}
	}
}

int main(int argc, char **argv)
{
	int rounds = argc > 1 ? atoi(argv[1]) : 300;
	srand(20261115);
	directed();
	random_history(rounds);
	printf("media_keys: %d checks, %d failed\n", s_checks, s_failed);
	return s_failed ? 1 : 0;
}
