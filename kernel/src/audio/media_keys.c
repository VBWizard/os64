// media_keys.c — which program a media key is for, as arithmetic.
// media_keys.h is the contract and MEDIA_KEYS.md the design; nothing here
// knows a lock, a thread or a clock.

#include "audio/media_keys.h"

static void zero(void *p, size_t n)
{
	uint8_t *b = p;
	for (size_t i = 0; i < n; i++)
		b[i] = 0;
}

void media_keys_table_init(media_keys_table_t *t)
{
	zero(t, sizeof(*t));
	t->last_slot = -1;
}

void media_keys_session_init(media_keys_session_t *s, const uint64_t *lineage, uint32_t n)
{
	zero(s, sizeof(*s));
	s->state = MEDIA_KEYS_STOPPED;
	if (n > MEDIA_KEYS_LINEAGE)
		n = MEDIA_KEYS_LINEAGE;
	for (uint32_t i = 0; i < n; i++)
		s->lineage[i] = lineage[i];
	s->lineage_len = n;
}

int media_keys_add(media_keys_table_t *t, media_keys_session_t *s)
{
	for (uint32_t i = 0; i < MEDIA_KEYS_SESSIONS; i++)
		if (t->slot[i] == NULL) {
			t->slot[i] = s;
			t->listed++;
			s->active_seq = ++t->seq;   // opening is activity: no two sessions ever tie
			return (int)i;
		}
	return -1;
}

void media_keys_remove(media_keys_table_t *t, media_keys_session_t *s)
{
	for (uint32_t i = 0; i < MEDIA_KEYS_SESSIONS; i++)
		if (t->slot[i] == s) {
			t->slot[i] = NULL;
			t->listed--;
			if (t->last_slot == (int32_t)i)
				t->last_slot = -1;
		}
}

static bool space(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static bool word_is(const char *text, size_t length, const char *word)
{
	size_t n = 0;
	while (word[n] != '\0')
		n++;
	if (length < n)
		return false;
	for (size_t i = 0; i < n; i++)
		if (text[i] != word[i])
			return false;
	for (size_t i = n; i < length; i++)
		if (!space(text[i]))
			return false;
	return true;
}

bool media_keys_parse_state(const char *text, size_t length, uint8_t *state)
{
	if (word_is(text, length, "playing"))
		*state = MEDIA_KEYS_PLAYING;
	else if (word_is(text, length, "paused"))
		*state = MEDIA_KEYS_PAUSED;
	else if (word_is(text, length, "stopped"))
		*state = MEDIA_KEYS_STOPPED;
	else
		return false;
	return true;
}

void media_keys_set_state(media_keys_table_t *t, media_keys_session_t *s, uint8_t state)
{
	uint64_t stamp = ++t->seq;
	if (state == MEDIA_KEYS_PLAYING && s->state != MEDIA_KEYS_PLAYING)
		s->playing_seq = stamp;
	s->state = state;
	s->active_seq = stamp;
}

// How near the focused window's owner is in a session's lineage: its place,
// or MEDIA_KEYS_LINEAGE when it is not there (or there is no focus).
static uint32_t nearness(const media_keys_session_t *s, uint64_t focus)
{
	if (focus == 0)
		return MEDIA_KEYS_LINEAGE;
	for (uint32_t i = 0; i < s->lineage_len; i++)
		if (s->lineage[i] == focus)
			return i;
	return MEDIA_KEYS_LINEAGE;
}

// The slot the rule picks, or -1, and which rule picked it. A session whose
// ring is full takes no part at any rule.
static int pick(media_keys_table_t *t, uint64_t focus, uint32_t *rule)
{
	int best = -1;
	for (uint32_t i = 0; i < MEDIA_KEYS_SESSIONS; i++) {
		media_keys_session_t *s = t->slot[i];
		if (s == NULL || s->count == MEDIA_KEYS_RING || s->state != MEDIA_KEYS_PLAYING)
			continue;
		if (best < 0 || s->playing_seq > t->slot[best]->playing_seq)
			best = (int)i;
	}
	if (best >= 0) {
		*rule = MEDIA_KEYS_RULE_PLAYING;
		return best;
	}
	uint32_t nearest = MEDIA_KEYS_LINEAGE;
	for (uint32_t i = 0; i < MEDIA_KEYS_SESSIONS; i++) {
		media_keys_session_t *s = t->slot[i];
		if (s == NULL || s->count == MEDIA_KEYS_RING)
			continue;
		uint32_t near = nearness(s, focus);
		if (near == MEDIA_KEYS_LINEAGE)
			continue;
		if (best < 0 || near < nearest || (near == nearest && s->active_seq > t->slot[best]->active_seq)) {
			best = (int)i;
			nearest = near;
		}
	}
	if (best >= 0) {
		*rule = MEDIA_KEYS_RULE_FOCUS;
		return best;
	}
	for (uint32_t i = 0; i < MEDIA_KEYS_SESSIONS; i++) {
		media_keys_session_t *s = t->slot[i];
		if (s == NULL || s->count == MEDIA_KEYS_RING)
			continue;
		if (best < 0 || s->active_seq > t->slot[best]->active_seq)
			best = (int)i;
	}
	*rule = best >= 0 ? MEDIA_KEYS_RULE_ACTIVE : MEDIA_KEYS_RULE_KERNEL;
	return best;
}

media_keys_route_t media_keys_press(media_keys_table_t *t, uint32_t key, uint64_t focus, uint64_t now_us)
{
	media_keys_route_t route = {NULL, 0, false};
	t->presses++;
	t->last_key = key;
	t->last_focus = focus;
	for (uint32_t i = 0; i < MEDIA_KEYS_SESSIONS; i++)
		if (t->slot[i] != NULL)
			t->slot[i]->passed_full = t->slot[i]->count == MEDIA_KEYS_RING;
	uint32_t rule = 0;
	int at = pick(t, focus, &rule);
	t->last_rule = rule;
	t->last_slot = at;
	bool toggle = key == OS64_MEDIA_KEY_PLAY || key == OS64_MEDIA_KEY_PAUSE;
	if (at < 0) {
		t->last_delivered = 0;
		if (toggle)
			route.kernel_toggle = true;
		else
			t->unanswered++;
		return route;
	}
	media_keys_session_t *s = t->slot[at];
	uint32_t given = key;
	if (toggle)
		given = s->state == MEDIA_KEYS_PLAYING ? OS64_MEDIA_KEY_PAUSE : OS64_MEDIA_KEY_PLAY;
	os64_media_key_t *slot = &s->ring[(s->head + s->count) % MEDIA_KEYS_RING];
	slot->key = given;
	slot->serial = ++s->serial;
	slot->at_us = now_us;
	s->count++;
	s->active_seq = ++t->seq;
	t->last_delivered = given;
	route.target = s;
	route.delivered = given;
	return route;
}

void media_keys_kernel_failed(media_keys_table_t *t)
{
	t->unanswered++;
}

bool media_keys_take(media_keys_session_t *s, os64_media_key_t *out)
{
	if (s->count == 0)
		return false;
	*out = s->ring[s->head];
	s->head = (s->head + 1) % MEDIA_KEYS_RING;
	s->count--;
	s->delivered++;
	return true;
}

void media_keys_undelivered(media_keys_session_t *s)
{
	s->delivered--;
	s->lost++;
}
