// test_audio_stream_host.c — kernel/src/audio/audio_stream.c beside a model
// that is plainly right: a list of frames each tagged with the generation
// that queued it, and a handful of integers. Random sequences of every event
// compare the pulled sound, the tap's records, every field of the position
// and the state after each step; the named cases pin each rule of
// docs/design/pending/AUDIO_SEAM.md by itself.
//
//   tools/test_audio_stream_host.sh [random rounds, default 300]

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio/audio_stream.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { failures++; \
	printf("FAIL %s:%d: %s\n", __func__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { long long a_ = (long long)(a), b_ = (long long)(b); if (a_ != b_) { failures++; \
	printf("FAIL %s:%d: %s = %lld, want %s = %lld\n", __func__, __LINE__, #a, a_, #b, b_); } } while (0)

// ── The model ───────────────────────────────────────────────────────────────

#define MCAP 64
typedef struct { int16_t l, r; uint32_t gen; } mframe_t;
typedef struct
{
	mframe_t q[MCAP];
	uint32_t n;
	uint8_t  part[4];
	uint32_t part_n;
	bool open, paused, written, failed, dry;
	uint32_t gen, prev_gen;
	uint64_t accepted, taken, underruns, stamp, given;
	uint32_t delay_us, volume;
	const void *sink;
} model_t;

static void m_restart(model_t *m)
{
	m->n = 0; m->part_n = 0; m->gen++;
	m->accepted = m->taken = m->underruns = 0;
	m->written = m->dry = false;
}

static uint32_t m_state(const model_t *m)
{
	if (m->failed) return OS64_AUDIO_FAILED_STATE;
	if (!m->open) return m->n ? OS64_AUDIO_DRAINING : OS64_AUDIO_IDLE;
	if (m->paused) return OS64_AUDIO_PAUSED;
	if (m->n) return OS64_AUDIO_PLAYING;
	return m->written ? OS64_AUDIO_STARVED : OS64_AUDIO_WAITING;
}

static void m_push(model_t *m, const uint8_t *f)
{
	m->q[m->n].l = (int16_t)(f[0] | (f[1] << 8));
	m->q[m->n].r = (int16_t)(f[2] | (f[3] << 8));
	m->q[m->n].gen = m->gen;
	m->n++; m->accepted++; m->written = true; m->dry = false;
}

static long m_write(model_t *m, const uint8_t *b, size_t len, uint32_t gen)
{
	if (m->failed) return OS64_AUDIO_FAILED;
	if (gen != m->gen) return OS64_AUDIO_FLUSHED;
	size_t used = 0;
	// Frame by frame: one is begun only if this write can finish it and
	// there is room for it, or if this write ends inside it.
	while (used < len)
	{
		size_t need = 4 - m->part_n;
		if (len - used < need)
		{
			while (used < len) m->part[m->part_n++] = b[used++];
			break;
		}
		if (m->n == MCAP) break;
		while (m->part_n < 4) m->part[m->part_n++] = b[used++];
		m_push(m, m->part);
		m->part_n = 0;
	}
	return (long)used;
}

static int16_t m_gain(int16_t x, uint32_t v)
{
	long long g = (long long)v * v * 32768 / 10000;
	return (int16_t)((long long)x * g / 32768);
}

typedef struct { uint32_t frames, gen; bool paused, yours; } mpull_t;

static mpull_t m_pull(model_t *m, const void *sink, int16_t *out, uint32_t frames, uint64_t now)
{
	mpull_t r = { 0, m->gen, false, true };
	if (sink != m->sink) { r.yours = false; return r; }
	if (m->paused) { r.paused = true; return r; }
	uint32_t n = 0;
	bool old = m->n > 0 && m->q[0].gen != m->gen;
	while (n < frames && n < m->n && (m->q[n].gen != m->gen) == old)
		n++;
	for (uint32_t i = 0; i < frames; i++)
	{
		out[i * 2]     = i < n ? m_gain(m->q[i].l, m->volume) : 0;
		out[i * 2 + 1] = i < n ? m_gain(m->q[i].r, m->volume) : 0;
	}
	if (old) r.gen = m->prev_gen; else m->taken += n;
	memmove(m->q, m->q + n, (m->n - n) * sizeof(mframe_t));
	m->n -= n;
	r.frames = n;
	m->stamp = now;
	m->given += frames;
	if (n < frames && m->n == 0 && m->open && m->written && !m->failed && !m->dry)
	{
		m->dry = true;
		m->underruns++;
	}
	return r;
}

// ── One stream and its model, side by side ──────────────────────────────────

static audio_stream_t S;
static model_t M;
static int16_t ring[MCAP * 2];
static uint8_t tap_store[1 << 16];
static int some_sink, other_sink;
static uint64_t now_us;
static uint8_t next_byte;

static void both_init(void)
{
	audio_stream_init(&S, ring, MCAP, OS64_AUDIO_RATE);
	memset(&M, 0, sizeof(M));
	M.volume = 100;
	now_us = 1000;
	CHECK(audio_stream_tap_open(&S, tap_store, sizeof(tap_store)));
}

static void compare(void)
{
	os64_audio_position_t p;
	audio_stream_position(&S, &p);
	uint64_t behind = (uint64_t)M.delay_us * OS64_AUDIO_RATE / 1000000u;
	CHECK_EQ(p.size, sizeof(p));
	CHECK_EQ(p.generation, M.gen);
	CHECK_EQ(p.state, m_state(&M));
	CHECK_EQ(audio_stream_state(&S), m_state(&M));
	CHECK_EQ(p.rate, OS64_AUDIO_RATE);
	CHECK_EQ(p.accepted, M.accepted);
	CHECK_EQ(p.taken, M.taken);
	CHECK_EQ(p.heard, M.taken > behind ? M.taken - behind : 0);
	CHECK_EQ(p.micros, M.stamp);
	CHECK_EQ(p.queued, M.n);
	CHECK_EQ(p.delay_us, M.delay_us);
	CHECK_EQ(p.underruns, M.underruns);
	CHECK(p.taken <= p.accepted);
	CHECK_EQ(audio_stream_has_work(&S), !M.failed && !M.paused && (M.open || M.n > 0));
	// Sound for the sink that is the sink, and for nobody else.
	CHECK_EQ(audio_stream_has_sound(&S, S.sink), !M.failed && !M.paused && M.n > 0);
	CHECK(!audio_stream_has_sound(&S, S.sink == (const void *)&other_sink ? (const void *)&some_sink : (const void *)&other_sink));
	CHECK_EQ(audio_stream_writer_should_wake(&S), MCAP - M.n >= MCAP / 4);
}

static void do_write(size_t len, uint32_t gen)
{
	uint8_t b[512];
	for (size_t i = 0; i < len; i++)
		b[i] = next_byte++ * 7 + 3;
	long a = audio_stream_write(&S, b, len, gen);
	long e = m_write(&M, b, len, gen);
	CHECK_EQ(a, e);
	if (a >= 0 && (size_t)a < len)
		next_byte -= (uint8_t)(len - (size_t)a);   // the caller would offer them again
}

// Pull from both, compare the sound, and check the tap said the same thing.
static void do_pull(const void *sink, uint32_t frames)
{
	int16_t a[MCAP * 4 + 8], e[MCAP * 4 + 8];
	memset(a, 0x55, sizeof(a));
	memset(e, 0x55, sizeof(e));
	now_us += 137;
	uint64_t given_before = M.given;
	audio_pulled_t ra = audio_stream_pull(&S, sink, a, frames, now_us);
	mpull_t re = m_pull(&M, sink, e, frames, now_us);
	CHECK_EQ(ra.yours, re.yours);
	CHECK_EQ(ra.paused, re.paused);
	CHECK_EQ(ra.frames, re.frames);
	CHECK_EQ(ra.generation, re.gen);
	CHECK(memcmp(a, e, sizeof(a)) == 0);   // the guard bytes past `frames` too

	static uint8_t got[sizeof(tap_store)];
	size_t bytes = audio_stream_tap_read(&S, got, sizeof(got));
	if (!re.yours || re.paused)
	{
		CHECK_EQ(bytes, 0);
		return;
	}
	size_t at = 0;
	uint32_t seen = 0;
	while (at < bytes)
	{
		os64_audio_tap_t h;
		memcpy(&h, got + at, sizeof(h));
		at += sizeof(h);
		CHECK(h.frames > 0 && h.frames <= OS64_AUDIO_TAP_RECORD_FRAMES);
		CHECK_EQ(h.first, given_before + seen);
		CHECK_EQ(h.micros, now_us);
		CHECK_EQ(h.dropped, 0);
		bool silent = seen >= re.frames;
		CHECK_EQ(h.silence, silent ? 1 : 0);
		CHECK_EQ(h.generation, silent ? M.gen : re.gen);
		CHECK(seen + h.frames <= (silent ? frames : re.frames));
		CHECK(memcmp(got + at, a + seen * 2, h.frames * 4) == 0);
		at += h.frames * 4;
		seen += h.frames;
	}
	CHECK_EQ(seen, frames);
}

static void random_rounds(unsigned rounds)
{
	for (unsigned round = 0; round < rounds; round++)
	{
		both_init();
		srand(round + 1);
		for (int step = 0; step < 400; step++)
		{
			switch (rand() % 16)
			{
				case 0: case 1: case 2: case 3: case 4:
					// A stale generation now and then, as a write that
					// straddled a flush would carry.
					do_write((size_t)(rand() % 97), rand() % 23 ? M.gen : M.gen - 1);
					break;
				case 5: case 6: case 7: case 8:
					do_pull(rand() % 19 ? M.sink : (M.sink == &other_sink ? &some_sink : &other_sink),
					        (uint32_t)(rand() % 40));
					break;
				case 9:
					CHECK_EQ(audio_stream_open(&S), !M.open);
					if (!M.open)
					{
						M.open = true; M.prev_gen = M.gen; M.gen++;
						M.accepted = M.taken = M.underruns = 0;
						M.written = M.dry = false; M.part_n = 0;
					}
					break;
				case 10:
					audio_stream_close(&S);
					if (M.open)
					{
						if (M.paused) { m_restart(&M); M.paused = false; }
						M.open = false; M.part_n = 0;
					}
					break;
				case 11:
					CHECK_EQ(audio_stream_pause(&S), M.open && !M.paused && !M.failed);
					if (M.open && !M.failed) M.paused = true;
					break;
				case 12:
					CHECK_EQ(audio_stream_resume(&S), M.paused);
					M.paused = false;
					break;
				case 13:
					audio_stream_flush(&S);
					if (M.open || M.n > 0) m_restart(&M);
					break;
				case 14:
				{
					uint32_t v = (uint32_t)(rand() % 120);
					CHECK_EQ(audio_stream_set_volume(&S, v), v <= 100);
					if (v <= 100) M.volume = v;
					uint32_t d = (uint32_t)(rand() % 3000);
					const void *who = rand() % 5 ? M.sink : &other_sink;
					audio_stream_sink_delay(&S, who, d);
					if (who == M.sink) M.delay_us = d;
					break;
				}
				case 15:
					switch (rand() % 6)
					{
						case 0:
							audio_stream_sink_attach(&S, &some_sink);
							M.sink = &some_sink; M.failed = false; M.delay_us = 0; m_restart(&M);
							break;
						case 1:
						{
							const void *who = rand() % 2 ? &some_sink : &other_sink;
							audio_stream_sink_detach(&S, who);
							if (who == M.sink) { M.sink = NULL; M.failed = false; M.delay_us = 0; m_restart(&M); }
							break;
						}
						case 2:
						{
							const void *who = rand() % 2 ? M.sink : &other_sink;
							audio_stream_sink_failed(&S, who);
							if (who == M.sink) { M.failed = true; M.n = 0; M.part_n = 0; }
							break;
						}
						default: break;
					}
					break;
			}
			compare();
		}
		audio_stream_tap_close(&S);
	}
}

// ── The named cases ─────────────────────────────────────────────────────────

static void frame_bytes(uint8_t *b, int16_t l, int16_t r)
{
	b[0] = (uint8_t)l; b[1] = (uint8_t)((uint16_t)l >> 8);
	b[2] = (uint8_t)r; b[3] = (uint8_t)((uint16_t)r >> 8);
}

static audio_stream_t T;
static int16_t tring[16];   // eight frames

static uint32_t t_open(void)
{
	audio_stream_init(&T, tring, 8, OS64_AUDIO_RATE);
	CHECK(audio_stream_open(&T));
	return T.generation;
}

static void t_write_ramp(uint32_t gen, int16_t from, int count)
{
	for (int i = 0; i < count; i++)
	{
		uint8_t b[4];
		frame_bytes(b, (int16_t)(from + i), (int16_t)~(from + i));
		CHECK_EQ(audio_stream_write(&T, b, 4, gen), 4);
	}
}

static void test_wrap_at_every_offset(void)
{
	for (int offset = 0; offset < 8; offset++)
	{
		uint32_t gen = t_open();
		int16_t out[16];
		t_write_ramp(gen, 100, offset);
		CHECK_EQ(audio_stream_pull(&T, NULL, out, (uint32_t)offset, 1).frames, offset);
		t_write_ramp(gen, 200, 8);
		uint8_t b[4] = { 0 };
		CHECK_EQ(audio_stream_write(&T, b, 4, gen), 0);   // full: not one byte
		CHECK_EQ(audio_stream_pull(&T, NULL, out, 8, 2).frames, 8);
		for (int i = 0; i < 8; i++)
		{
			CHECK_EQ(out[i * 2], 200 + i);
			CHECK_EQ(out[i * 2 + 1], (int16_t)~(200 + i));
		}
	}
}

static void test_a_frame_split_at_each_place(void)
{
	for (size_t cut = 1; cut < 4; cut++)
	{
		uint32_t gen = t_open();
		uint8_t b[8];
		frame_bytes(b, 0x1234, -0x1235);
		frame_bytes(b + 4, 0x0777, -0x0778);
		CHECK_EQ(audio_stream_write(&T, b, cut, gen), (long)cut);
		os64_audio_position_t p;
		audio_stream_position(&T, &p);
		CHECK_EQ(p.accepted, 0);
		CHECK_EQ(p.state, OS64_AUDIO_WAITING);
		CHECK_EQ(audio_stream_write(&T, b + cut, 8 - cut, gen), (long)(8 - cut));
		int16_t out[4];
		CHECK_EQ(audio_stream_pull(&T, NULL, out, 2, 1).frames, 2);
		CHECK_EQ(out[0], 0x1234); CHECK_EQ(out[1], -0x1235);
		CHECK_EQ(out[2], 0x0777); CHECK_EQ(out[3], -0x0778);
	}
}

static void test_a_finished_split_frame_waits_for_room(void)
{
	uint32_t gen = t_open();
	uint8_t b[4];
	frame_bytes(b, 9, 10);
	CHECK_EQ(audio_stream_write(&T, b, 2, gen), 2);
	uint8_t rest[2 + 4 * 8];
	rest[0] = b[2]; rest[1] = b[3];
	for (int i = 0; i < 8; i++)
		frame_bytes(rest + 2 + i * 4, (int16_t)(50 + i), 0);
	// The split frame and seven more fit; the eighth does not.
	CHECK_EQ(audio_stream_write(&T, rest, sizeof(rest), gen), 2 + 4 * 7);
	int16_t out[16];
	CHECK_EQ(audio_stream_pull(&T, NULL, out, 8, 1).frames, 8);
	CHECK_EQ(out[0], 9); CHECK_EQ(out[1], 10);
	CHECK_EQ(out[2], 50); CHECK_EQ(out[14], 56);

	// A split frame against a full ring: the bytes that would finish it are
	// not taken, since a byte counted is a byte that will be played; fewer
	// than would finish it are, since they need no room yet.
	uint8_t eight[32];
	for (int i = 0; i < 8; i++)
		frame_bytes(eight + i * 4, (int16_t)(70 + i), 0);
	gen = t_open();
	CHECK_EQ(audio_stream_write(&T, eight, 32, gen), 32);
	CHECK_EQ(audio_stream_write(&T, b, 1, gen), 1);          // begins a frame; the ring is full
	CHECK_EQ(audio_stream_write(&T, b + 1, 1, gen), 1);
	CHECK_EQ(audio_stream_write(&T, b + 2, 2, gen), 0);      // would finish it: nothing taken
	CHECK_EQ(audio_stream_write(&T, eight, 32, gen), 0);
	os64_audio_position_t p;
	audio_stream_position(&T, &p);
	CHECK_EQ(p.accepted, 8);
	CHECK_EQ(audio_stream_pull(&T, NULL, out, 1, 1).frames, 1);
	CHECK_EQ(audio_stream_write(&T, b + 2, 6 - 4, gen), 2);  // room: finished and placed
	audio_stream_position(&T, &p);
	CHECK_EQ(p.accepted, 9);
	CHECK_EQ(p.queued, 8);
	// A close with a frame begun and unfinished drops those bytes and no
	// whole frame: everything counted as accepted is still in the ring.
	CHECK_EQ(audio_stream_write(&T, b, 3, gen), 3);
	audio_stream_close(&T);
	CHECK_EQ(T.fill, 8);
	CHECK_EQ(T.split_len, 0);
	CHECK_EQ(audio_stream_pull(&T, NULL, out, 8, 2).frames, 8);
	CHECK_EQ(out[14], 9); CHECK_EQ(out[15], 10);             // the split frame, last in
}

static void test_flush_and_generations(void)
{
	uint32_t gen = t_open();
	uint8_t b[4];
	frame_bytes(b, 1, 2);
	t_write_ramp(gen, 0, 5);
	CHECK_EQ(audio_stream_write(&T, b, 3, gen), 3);
	audio_stream_flush(&T);
	os64_audio_position_t p;
	audio_stream_position(&T, &p);
	CHECK_EQ(p.generation, gen + 1);
	CHECK_EQ(p.queued, 0);
	CHECK_EQ(p.accepted, 0);
	CHECK_EQ(p.state, OS64_AUDIO_WAITING);
	// The write that straddled it, and one made with the new number.
	CHECK_EQ(audio_stream_write(&T, b, 4, gen), OS64_AUDIO_FLUSHED);
	CHECK_EQ(audio_stream_write(&T, b, 4, gen + 1), 4);
	int16_t out[2];
	CHECK_EQ(audio_stream_pull(&T, NULL, out, 1, 1).frames, 1);
	CHECK_EQ(out[0], 1);   // the old split frame's three bytes did not survive into it

	// A flush with nothing open and nothing queued changes nothing.
	audio_stream_close(&T);
	audio_stream_position(&T, &p);
	CHECK_EQ(p.state, OS64_AUDIO_IDLE);
	uint32_t before = p.generation;
	audio_stream_flush(&T);
	audio_stream_position(&T, &p);
	CHECK_EQ(p.generation, before);

	// Draining, a flush empties and moves on.
	gen = T.generation;
	CHECK(audio_stream_open(&T));
	t_write_ramp(T.generation, 0, 3);
	audio_stream_close(&T);
	CHECK_EQ(audio_stream_state(&T), OS64_AUDIO_DRAINING);
	gen = T.generation;
	audio_stream_flush(&T);
	CHECK_EQ(audio_stream_state(&T), OS64_AUDIO_IDLE);
	CHECK_EQ(T.generation, gen + 1);
}

static void test_open_while_draining(void)
{
	uint32_t first = t_open();
	t_write_ramp(first, 10, 3);
	audio_stream_close(&T);
	CHECK(audio_stream_open(&T));
	CHECK(!audio_stream_open(&T));
	uint32_t second = T.generation;
	CHECK_EQ(second, first + 1);
	os64_audio_position_t p;
	audio_stream_position(&T, &p);
	CHECK_EQ(p.state, OS64_AUDIO_PLAYING);   // the earlier owner's sound is playing
	CHECK_EQ(p.queued, 3);
	CHECK_EQ(p.accepted, 0);
	t_write_ramp(second, 20, 2);

	// The pull that ends the old frames is silence from there, never the
	// new owner's frames, and counts nothing for the new owner.
	int16_t out[8];
	audio_pulled_t r = audio_stream_pull(&T, NULL, out, 4, 5);
	CHECK_EQ(r.frames, 3);
	CHECK_EQ(r.generation, first);
	CHECK_EQ(out[0], 10); CHECK_EQ(out[4], 12); CHECK_EQ(out[6], 0);
	audio_stream_position(&T, &p);
	CHECK_EQ(p.taken, 0);
	CHECK_EQ(p.underruns, 0);
	r = audio_stream_pull(&T, NULL, out, 4, 6);
	CHECK_EQ(r.frames, 2);
	CHECK_EQ(r.generation, second);
	CHECK_EQ(out[0], 20); CHECK_EQ(out[2], 21);
	audio_stream_position(&T, &p);
	CHECK_EQ(p.taken, 2);
	CHECK_EQ(p.underruns, 1);

	// The old frames run out before the new owner writes: that is waiting,
	// not an underrun.
	first = t_open();
	t_write_ramp(first, 10, 2);
	audio_stream_close(&T);
	CHECK(audio_stream_open(&T));
	CHECK_EQ(audio_stream_pull(&T, NULL, out, 4, 7).frames, 2);
	audio_stream_position(&T, &p);
	CHECK_EQ(p.state, OS64_AUDIO_WAITING);
	CHECK_EQ(p.underruns, 0);
	CHECK_EQ(audio_stream_pull(&T, NULL, out, 4, 8).frames, 0);
	audio_stream_position(&T, &p);
	CHECK_EQ(p.underruns, 0);
}

static void test_underruns_are_counted_once_a_spell(void)
{
	uint32_t gen = t_open();
	int16_t out[16];
	os64_audio_position_t p;
	CHECK_EQ(audio_stream_pull(&T, NULL, out, 4, 1).frames, 0);   // waiting
	audio_stream_position(&T, &p);
	CHECK_EQ(p.underruns, 0);
	t_write_ramp(gen, 0, 4);
	CHECK_EQ(audio_stream_pull(&T, NULL, out, 4, 2).frames, 4);   // emptied exactly: no shortfall yet
	audio_stream_position(&T, &p);
	CHECK_EQ(p.underruns, 0);
	CHECK_EQ(p.state, OS64_AUDIO_STARVED);
	CHECK_EQ(audio_stream_pull(&T, NULL, out, 4, 3).frames, 0);
	CHECK_EQ(audio_stream_pull(&T, NULL, out, 4, 4).frames, 0);
	audio_stream_position(&T, &p);
	CHECK_EQ(p.underruns, 1);
	// Paused through the dry spell and resumed into it: still the one.
	CHECK(audio_stream_pause(&T));
	CHECK(audio_stream_pull(&T, NULL, out, 4, 5).paused);
	CHECK(audio_stream_resume(&T));
	CHECK_EQ(audio_stream_pull(&T, NULL, out, 4, 6).frames, 0);
	audio_stream_position(&T, &p);
	CHECK_EQ(p.underruns, 1);
	t_write_ramp(gen, 0, 1);
	CHECK_EQ(audio_stream_pull(&T, NULL, out, 4, 7).frames, 1);   // part ring, part silence
	audio_stream_position(&T, &p);
	CHECK_EQ(p.underruns, 2);
	CHECK_EQ(T.total_underruns, 2);
	CHECK_EQ(T.total_silence, 4 + 4 + 4 + 4 + 3);
	// Closed, the ring running out is the end of the sound, not a fault.
	t_write_ramp(gen, 0, 1);
	audio_stream_close(&T);
	CHECK_EQ(audio_stream_pull(&T, NULL, out, 4, 8).frames, 1);
	CHECK_EQ(T.total_underruns, 2);
	CHECK_EQ(audio_stream_state(&T), OS64_AUDIO_IDLE);
}

static void test_pause_freezes_and_refuses(void)
{
	audio_stream_init(&T, tring, 8, OS64_AUDIO_RATE);
	CHECK(!audio_stream_pause(&T));           // nothing open
	CHECK(!audio_stream_resume(&T));
	uint32_t gen = t_open();
	t_write_ramp(gen, 0, 6);
	int16_t out[16];
	CHECK_EQ(audio_stream_pull(&T, NULL, out, 2, 50).frames, 2);
	CHECK(audio_stream_pause(&T));
	CHECK(!audio_stream_pause(&T));
	out[0] = 0x7777;
	audio_pulled_t r = audio_stream_pull(&T, NULL, out, 2, 99);
	CHECK(r.paused);
	CHECK_EQ(r.frames, 0);
	CHECK_EQ(out[0], 0x7777);                 // untouched
	os64_audio_position_t p;
	audio_stream_position(&T, &p);
	CHECK_EQ(p.taken, 2);
	CHECK_EQ(p.micros, 50);                   // the last real pull's
	CHECK_EQ(p.state, OS64_AUDIO_PAUSED);
	CHECK_EQ(T.total_given, 2);
	t_write_ramp(gen, 0, 4);                  // a paused stream still takes writes
	CHECK(!audio_stream_has_work(&T));

	// Closing a paused stream throws its sound away and lifts the pause.
	audio_stream_close(&T);
	audio_stream_position(&T, &p);
	CHECK_EQ(p.state, OS64_AUDIO_IDLE);
	CHECK_EQ(p.generation, gen + 1);
	CHECK(audio_stream_open(&T));
	CHECK_EQ(audio_stream_state(&T), OS64_AUDIO_WAITING);
}

static void test_gain(void)
{
	static const int16_t samples[] = { 32767, -32768, 1, -1, 12345, -12345, 0, 3, -3 };
	static const struct { uint32_t volume; int32_t q15; } steps[] = {
		{ 0, 0 }, { 1, 3 }, { 50, 8192 }, { 99, 32115 }, { 100, 32768 },
	};
	for (size_t v = 0; v < sizeof(steps) / sizeof(steps[0]); v++)
	{
		uint32_t gen = t_open();
		CHECK(audio_stream_set_volume(&T, steps[v].volume));
		CHECK_EQ(T.gain_q15, steps[v].q15);
		for (size_t i = 0; i + 1 < sizeof(samples) / sizeof(samples[0]); i += 2)
		{
			uint8_t b[4];
			frame_bytes(b, samples[i], samples[i + 1]);
			CHECK_EQ(audio_stream_write(&T, b, 4, gen), 4);
			int16_t out[2];
			CHECK_EQ(audio_stream_pull(&T, NULL, out, 1, 1).frames, 1);
			for (int c = 0; c < 2; c++)
			{
				int want = (int)((long long)samples[i + c] * steps[v].q15 / 32768);
				CHECK_EQ(out[c], want);
			}
		}
	}
	uint32_t gen = t_open();
	(void)gen;
	CHECK(audio_stream_set_volume(&T, 50));
	CHECK(!audio_stream_set_volume(&T, 101));
	CHECK_EQ(T.volume, 50);
	// The gain is the pull's: a change is heard in what was already queued.
	t_write_ramp(T.generation, 1000, 2);
	int16_t out[2];
	audio_stream_pull(&T, NULL, out, 1, 1);
	CHECK_EQ(out[0], 250);
	CHECK(audio_stream_set_volume(&T, 100));
	audio_stream_pull(&T, NULL, out, 1, 2);
	CHECK_EQ(out[0], 1001);
	// -1 at half volume is 0, not -1: toward zero.
	t_write_ramp(T.generation, -1, 1);
	CHECK(audio_stream_set_volume(&T, 50));
	audio_stream_pull(&T, NULL, out, 1, 3);
	CHECK_EQ(out[0], 0);
}

static void test_heard_is_taken_less_the_delay(void)
{
	uint32_t gen = t_open();
	audio_stream_sink_delay(&T, NULL, 125);   // six frames at 48 kHz
	t_write_ramp(gen, 0, 8);
	int16_t out[16];
	os64_audio_position_t p;
	audio_stream_pull(&T, NULL, out, 4, 10);
	audio_stream_position(&T, &p);
	CHECK_EQ(p.heard, 0);                     // a delay larger than what was taken
	CHECK_EQ(p.delay_us, 125);
	audio_stream_pull(&T, NULL, out, 4, 20);
	audio_stream_position(&T, &p);
	CHECK_EQ(p.taken, 8);
	CHECK_EQ(p.heard, 2);
	CHECK_EQ(p.micros, 20);
	audio_stream_sink_delay(&T, &some_sink, 0);   // not the sink: ignored
	audio_stream_position(&T, &p);
	CHECK_EQ(p.delay_us, 125);
}

static void test_sinks(void)
{
	uint32_t gen = t_open();
	t_write_ramp(gen, 0, 4);
	int16_t out[8];
	out[0] = 0x1111;
	// A pump that is not the sink takes nothing and marks nothing.
	audio_pulled_t r = audio_stream_pull(&T, &some_sink, out, 2, 77);
	CHECK(!r.yours);
	CHECK_EQ(out[0], 0x1111);
	CHECK_EQ(T.refused_pulls, 1);
	CHECK_EQ(T.fill, 4);
	CHECK_EQ(T.taken_micros, 0);
	// A change of sink is a flush, whichever way it goes.
	audio_stream_sink_delay(&T, NULL, 900);
	audio_stream_sink_attach(&T, &some_sink);
	os64_audio_position_t p;
	audio_stream_position(&T, &p);
	CHECK_EQ(p.generation, gen + 1);
	CHECK_EQ(p.queued, 0);
	CHECK_EQ(p.delay_us, 0);
	CHECK(!audio_stream_pull(&T, NULL, out, 2, 78).yours);       // the null sink's pump, now a stranger
	CHECK(audio_stream_pull(&T, &some_sink, out, 2, 79).yours);
	// Sound is frames in the ring, unpaused, asked by the sink: an open with
	// nothing written is none, and a stranger is never told there is some.
	CHECK(!audio_stream_has_sound(&T, &some_sink));
	t_write_ramp(gen + 1, 0, 2);
	CHECK(audio_stream_has_sound(&T, &some_sink));
	CHECK(!audio_stream_has_sound(&T, NULL));
	CHECK(!audio_stream_has_sound(&T, &other_sink));
	CHECK(audio_stream_pause(&T));
	CHECK(!audio_stream_has_sound(&T, &some_sink));
	CHECK(audio_stream_resume(&T));
	CHECK(audio_stream_has_sound(&T, &some_sink));
	CHECK_EQ(audio_stream_pull(&T, &some_sink, out, 2, 79).frames, 2);
	CHECK(!audio_stream_has_sound(&T, &some_sink));
	audio_stream_sink_detach(&T, &other_sink);                    // not the sink: ignored
	CHECK_EQ(T.generation, gen + 1);
	audio_stream_sink_failed(&T, &other_sink);
	CHECK(!T.failed);

	// Failed closed: the ring is emptied, writes answer for it, a pull is
	// silence, pause is refused, and an open is still granted.
	t_write_ramp(gen + 1, 0, 3);
	audio_stream_sink_failed(&T, &some_sink);
	CHECK_EQ(audio_stream_state(&T), OS64_AUDIO_FAILED_STATE);
	uint8_t b[4] = { 1, 2, 3, 4 };
	CHECK_EQ(audio_stream_write(&T, b, 4, gen + 1), OS64_AUDIO_FAILED);
	CHECK_EQ(audio_stream_write(&T, b, 4, gen), OS64_AUDIO_FAILED);   // failed outranks flushed
	CHECK(audio_stream_writer_should_wake(&T));
	CHECK(!audio_stream_has_work(&T));
	r = audio_stream_pull(&T, &some_sink, out, 2, 80);
	CHECK_EQ(r.frames, 0);
	CHECK_EQ(out[0], 0);
	CHECK(!audio_stream_pause(&T));
	audio_stream_close(&T);
	CHECK(audio_stream_open(&T));
	CHECK_EQ(audio_stream_state(&T), OS64_AUDIO_FAILED_STATE);
	CHECK_EQ(T.underruns, 0);
	// The failure leaves with its sink.
	audio_stream_sink_detach(&T, &some_sink);
	CHECK_EQ(audio_stream_state(&T), OS64_AUDIO_WAITING);
	CHECK(T.sink == NULL);
}

static void test_clock(void)
{
	bool slip;
	CHECK_EQ(audio_clock_due(1000, 1000, 0, 48000, 9600, &slip), 0);
	CHECK_EQ(audio_clock_due(1000, 900, 0, 48000, 9600, &slip), 0);       // a clock behind its anchor
	CHECK_EQ(audio_clock_due(1000, 11000, 0, 48000, 9600, &slip), 480);   // one tick
	CHECK(!slip);
	CHECK_EQ(audio_clock_due(1000, 11000, 480, 48000, 9600, &slip), 0);
	CHECK_EQ(audio_clock_due(1000, 11000, 500, 48000, 9600, &slip), 0);   // ahead: nothing, not a wrap
	CHECK_EQ(audio_clock_due(1000, 21000, 480, 48000, 9600, &slip), 480);
	CHECK_EQ(audio_clock_due(0, 200000, 0, 48000, 9600, &slip), 9600);    // exactly the ring: no slip
	CHECK(!slip);
	CHECK_EQ(audio_clock_due(0, 200021, 0, 48000, 9600, &slip), 9600);    // one frame over
	CHECK(slip);
	// A month of playing loses nothing to the arithmetic.
	uint64_t month = 30ull * 24 * 3600 * 1000000;
	CHECK_EQ(audio_clock_due(0, month, month / 1000000 * 48000 - 7, 48000, 9600, &slip), 7);
	CHECK(!slip);
	// 44.1 kHz does not drift either: over ten seconds, exactly 441,000.
	uint64_t taken = 0;
	for (uint64_t t = 10000; t <= 10000000; t += 10000)
		taken += audio_clock_due(0, t, taken, 44100, 9600, &slip);
	CHECK_EQ(taken, 441000);
}

static void test_tap(void)
{
	static uint8_t store[32 + 4 * 6 + 32 + 4 * 2];   // room for a six-frame record and a two-frame one
	uint32_t gen = t_open();
	CHECK(audio_stream_tap_open(&T, store, sizeof(store)));
	CHECK(!audio_stream_tap_open(&T, store, sizeof(store)));
	int16_t out[16];
	uint8_t got[256];
	CHECK_EQ(audio_stream_tap_read(&T, got, sizeof(got)), 0);

	t_write_ramp(gen, 30, 6);
	audio_stream_pull(&T, NULL, out, 8, 5);   // six of sound, two of silence: both fit exactly
	t_write_ramp(gen, 40, 1);
	audio_stream_pull(&T, NULL, out, 1, 6);   // no room: dropped whole
	audio_stream_pull(&T, NULL, out, 1, 7);
	CHECK_EQ(T.tap_dropped_total, 2);

	// A reader with room for one record gets one, whole.
	CHECK_EQ(audio_stream_tap_read(&T, got, 32 + 4 * 6 + 5), 32 + 4 * 6);
	os64_audio_tap_t h;
	memcpy(&h, got, sizeof(h));
	CHECK_EQ(h.frames, 6); CHECK_EQ(h.silence, 0); CHECK_EQ(h.first, 0);
	CHECK_EQ(h.generation, gen); CHECK_EQ(h.micros, 5); CHECK_EQ(h.dropped, 0);
	int16_t pcm[12];
	memcpy(pcm, got + 32, sizeof(pcm));
	CHECK_EQ(pcm[0], 30); CHECK_EQ(pcm[10], 35);
	// ...and one too small for the next gets nothing and loses nothing.
	CHECK_EQ(audio_stream_tap_read(&T, got, 32 + 4 * 2 - 1), 0);
	CHECK_EQ(audio_stream_tap_read(&T, got, sizeof(got)), 32 + 4 * 2);
	memcpy(&h, got, sizeof(h));
	CHECK_EQ(h.frames, 2); CHECK_EQ(h.silence, 1); CHECK_EQ(h.first, 6);

	// The next record that gets in says what was lost, and wraps the store.
	audio_stream_tap_lost(&T, 1);
	t_write_ramp(gen, 50, 3);
	audio_stream_pull(&T, NULL, out, 3, 9);
	CHECK_EQ(audio_stream_tap_read(&T, got, sizeof(got)), 32 + 4 * 3);
	memcpy(&h, got, sizeof(h));
	CHECK_EQ(h.dropped, 3);
	CHECK_EQ(h.first, 10);   // the two dropped pulls were still given to the sink
	memcpy(pcm, got + 32, 12);
	CHECK_EQ(pcm[0], 50); CHECK_EQ(pcm[4], 52);
	audio_stream_pull(&T, NULL, out, 1, 10);
	CHECK_EQ(audio_stream_tap_read(&T, got, sizeof(got)), 32 + 4);
	memcpy(&h, got, sizeof(h));
	CHECK_EQ(h.dropped, 0);

	CHECK(audio_stream_tap_close(&T) == store);
	CHECK(audio_stream_tap_close(&T) == NULL);
	audio_stream_pull(&T, NULL, out, 1, 11);   // nobody listening: nothing is written anywhere
	CHECK_EQ(audio_stream_tap_read(&T, got, sizeof(got)), 0);

	// A pull larger than a record is cut into records of the largest size.
	static int16_t bigring[2 * 3000];
	static uint8_t bigstore[1 << 15];
	static int16_t bigout[2 * 3000];
	static uint8_t biggot[1 << 15];
	audio_stream_init(&T, bigring, 3000, OS64_AUDIO_RATE);
	CHECK(audio_stream_open(&T));
	CHECK(audio_stream_tap_open(&T, bigstore, sizeof(bigstore)));
	for (int i = 0; i < 2500; i++)
	{
		uint8_t b[4];
		frame_bytes(b, (int16_t)i, 0);
		audio_stream_write(&T, b, 4, T.generation);
	}
	audio_stream_pull(&T, NULL, bigout, 3000, 1);
	size_t bytes = audio_stream_tap_read(&T, biggot, sizeof(biggot));
	static const uint32_t want[] = { 1024, 1024, 452, 500 };
	size_t at = 0;
	uint64_t index = 0;
	for (int i = 0; i < 4; i++)
	{
		memcpy(&h, biggot + at, sizeof(h));
		CHECK_EQ(h.frames, want[i]);
		CHECK_EQ(h.silence, i == 3);
		CHECK_EQ(h.first, index);          // each cut piece starts where the last ended,
		int16_t left;
		memcpy(&left, biggot + at + sizeof(h), sizeof(left));
		CHECK_EQ(left, i == 3 ? 0 : (int16_t)index);   // ...in the sound as in the index
		index += h.frames;
		at += sizeof(h) + h.frames * 4;
	}
	CHECK_EQ(at, bytes);
}

int main(int argc, char **argv)
{
	unsigned rounds = argc > 1 ? (unsigned)atoi(argv[1]) : 300;

	test_wrap_at_every_offset();
	test_a_frame_split_at_each_place();
	test_a_finished_split_frame_waits_for_room();
	test_flush_and_generations();
	test_open_while_draining();
	test_underruns_are_counted_once_a_spell();
	test_pause_freezes_and_refuses();
	test_gain();
	test_heard_is_taken_less_the_delay();
	test_sinks();
	test_clock();
	test_tap();
	random_rounds(rounds);

	if (failures)
	{
		printf("test_audio_stream_host: %d FAILURE(S)\n", failures);
		return 1;
	}
	printf("test_audio_stream_host: all passed (%u random rounds)\n", rounds);
	return 0;
}
