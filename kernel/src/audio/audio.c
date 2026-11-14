// audio.c — the lock, the waits and the names around the one stream.
// audio.h is the contract; the arithmetic is audio_stream.c's.

#include "audio/audio.h"
#include "spinlock.h"
#include "kmalloc.h"
#include "memset.h"
#include "memcpy.h"
#include "smp_core.h"
#include "scheduler.h"
#include "signals.h"
#include "kernel.h"          // kTicksSinceStart
#include "CONFIG.h"          // TICKS_PER_SECOND
#include "micros.h"
#include "strings/sprintf.h"
#include "strings/strings.h"

struct audio_open
{
	audio_face_t face;
	uint32_t handles;   // under s_lock
	uint32_t refs;      // atomic
	bool     closed;    // under s_lock: the last handle went
};

static spinlock_t     s_lock;
static audio_stream_t s_stream;

// The one thread parked in a write, and the one parked in a tap read. Each
// registers under s_lock and voids its own slot at the top of every pass of
// its loop; a deliverer reads the slot under the same lock and wakes or
// owes (WAKE_OWED.md).
static thread_t *s_writer;
static thread_t *s_listener;

static const audio_sink_t *s_sink;   // NULL: the null sink
static const char *s_failed_why;

// The null sink's clock: where it counts from, and what it has taken since.
static uint64_t s_anchor_us;
static uint64_t s_anchor_taken;

volatile bool kAudioClocked;

// For /sys/audio.
static char     s_owner[32];
static uint64_t s_clock_slips;
static uint64_t s_writer_parks;
static uint64_t s_writer_wait_max_us;
static uint64_t s_pump_wakes;
static uint64_t s_pump_last_us;
static uint64_t s_pump_gap_max_us;
static uint64_t s_underruns_at_read;
static uint64_t s_pump_gap_max_since_read_us;

// How long a park lasts with nobody to end it. Every event that can end a
// wait wakes it; this is for the one that should have and did not.
#define AUDIO_BACKSTOP_TICKS TICKS_PER_SECOND

// The most a tap read takes in one hold of the lock.
#define AUDIO_TAP_READ_MAX (16 * OS64_AUDIO_TAP_READ_MIN)

void audio_init(void)
{
	int16_t *ring = kmalloc(AUDIO_RING_FRAMES * OS64_AUDIO_FRAME_BYTES);
	audio_stream_init(&s_stream, ring, ring != NULL ? AUDIO_RING_FRAMES : 0, OS64_AUDIO_RATE);
}

// After anything that changed the stream, s_lock held: republish what the
// tick reads, and start the null sink's clock when it goes from idle to
// running, so it never counts from an earlier stream's anchor.
static void republish_locked(void)
{
	bool clocked = s_sink == NULL && audio_stream_has_work(&s_stream);
	if (clocked && !kAudioClocked)
	{
		s_anchor_us = micros_now();
		s_anchor_taken = 0;
		s_pump_last_us = 0;
	}
	kAudioClocked = clocked;
}

static void wake_writer_locked(void)
{
	if (s_writer != NULL)
		scheduler_wake_or_owe(s_writer);
}

static void wake_listener_locked(void)
{
	if (s_listener != NULL)
		scheduler_wake_or_owe(s_listener);
}

static void nudge_sink_locked(void)
{
	if (s_sink != NULL && s_sink->nudge != NULL)
		s_sink->nudge(s_sink->ctx);
}

// ── The handle ──────────────────────────────────────────────────────────────

static audio_open_t *s_stream_open;   // the open that holds /dev/audio, under s_lock
static audio_open_t *s_tap_open;

audio_open_t *audio_open(audio_face_t face, long *refusal)
{
	audio_open_t *o = kmalloc(sizeof(*o));
	uint8_t *capture = face == AUDIO_FACE_TAP ? kmalloc(AUDIO_TAP_BYTES) : NULL;
	*refusal = -1;
	if (o == NULL || (face == AUDIO_FACE_TAP && capture == NULL) || s_stream.capacity == 0)
	{
		if (o) kfree(o);
		if (capture) kfree(capture);
		return NULL;
	}
	o->face = face;
	o->handles = 1;
	o->refs = 1;
	o->closed = false;

	const char *opener = ((task_t *)get_core_local_storage()->currentThread->ownerTask)->exename;

	bool granted = true;
	uint64_t flags = spinlock_acquire_irqsave(&s_lock);
	if (face == AUDIO_FACE_STREAM)
	{
		granted = audio_stream_open(&s_stream);
		if (granted)
		{
			s_stream_open = o;
			strncpy(s_owner, opener, sizeof(s_owner) - 1);
			s_owner[sizeof(s_owner) - 1] = '\0';
			republish_locked();
			nudge_sink_locked();
		}
	}
	else if (face == AUDIO_FACE_TAP)
	{
		granted = audio_stream_tap_open(&s_stream, capture, AUDIO_TAP_BYTES);
		if (granted)
			s_tap_open = o;
	}
	spinlock_release_irqrestore(&s_lock, flags);

	if (!granted)
	{
		kfree(o);
		if (capture) kfree(capture);
		*refusal = OS64_AUDIO_BUSY;
		return NULL;
	}
	return o;
}

audio_face_t audio_open_face(const audio_open_t *o)
{
	return o->face;
}

void audio_open_ref(audio_open_t *o)
{
	__sync_fetch_and_add(&o->refs, 1);
}

void audio_open_release(audio_open_t *o)
{
	if (__sync_sub_and_fetch(&o->refs, 1) == 0)
		kfree(o);
}

bool audio_open_share(audio_open_t *o)
{
	if (o->face != AUDIO_FACE_STREAM)
		return false;
	uint64_t flags = spinlock_acquire_irqsave(&s_lock);
	bool live = !o->closed;
	if (live)
	{
		o->handles++;
		audio_open_ref(o);
	}
	spinlock_release_irqrestore(&s_lock, flags);
	return live;
}

void audio_open_close(audio_open_t *o)
{
	uint8_t *capture = NULL;
	uint64_t flags = spinlock_acquire_irqsave(&s_lock);
	if (--o->handles == 0)
	{
		o->closed = true;
		if (o->face == AUDIO_FACE_STREAM && s_stream_open == o)
		{
			uint32_t before = s_stream.generation;
			audio_stream_close(&s_stream);
			s_stream_open = NULL;
			s_owner[0] = '\0';
			republish_locked();
			if (s_stream.generation != before)
				nudge_sink_locked();   // a paused stream's close is a flush
			// A sibling parked in a write on this open: it finds `closed`
			// (or the flush). The slot is the stream's, not this open's,
			// so it is emptied here: the next open's first write must not
			// find a thread of the last one still named in it.
			wake_writer_locked();
			s_writer = NULL;
		}
		else if (o->face == AUDIO_FACE_TAP && s_tap_open == o)
		{
			// Taken back under the lock a pull appends under, so no pull
			// is inside the storage when it is freed below.
			capture = audio_stream_tap_close(&s_stream);
			s_tap_open = NULL;
			wake_listener_locked();
			s_listener = NULL;   // the next open's, as the writer's slot above
		}
	}
	spinlock_release_irqrestore(&s_lock, flags);
	if (capture != NULL)
		kfree(capture);
	audio_open_release(o);
}

// ── The write ───────────────────────────────────────────────────────────────

uint32_t audio_write_begin(audio_open_t *o)
{
	(void)o;
	return s_stream.generation;   // one word; a flush racing this read is found by the first piece
}

long audio_write(audio_open_t *o, const uint8_t *bytes, size_t length,
                 uint32_t generation, uint64_t deadline)
{
	if (o->face != AUDIO_FACE_STREAM)
		return AUDIO_ERR_CLOSED;

	thread_t *self = get_core_local_storage()->currentThread;
	size_t placed = 0;
	uint64_t parked_at = 0;

	for (;;)
	{
		uint64_t flags = spinlock_acquire_irqsave(&s_lock);

		// Awake at the top: a registration left by the park we came out of
		// is void, and so is a wake owed for it. What it announced is in
		// the stream, and the checks below find it.
		if (s_writer == self)
			s_writer = NULL;
		scheduler_forget_owed_wake(self);
		if (parked_at != 0)
		{
			uint64_t waited = micros_now() - parked_at;
			if (waited > s_writer_wait_max_us)
				s_writer_wait_max_us = waited;
			parked_at = 0;
		}

		// A failed sink or a flush outranks every other way out: nothing
		// this call placed survives either, so the count it has is not an
		// answer, whatever else also happened while it was parked.
		if (s_stream.failed || s_stream.generation != generation)
		{
			long gone = s_stream.failed ? OS64_AUDIO_FAILED : OS64_AUDIO_FLUSHED;
			spinlock_release_irqrestore(&s_lock, flags);
			return gone;
		}

		long end = 0;
		if (o->closed)
			end = AUDIO_ERR_CLOSED;
		else if (signal_park_must_end(self))
			end = AUDIO_ERR_INTERRUPTED;
		else if (s_writer != NULL)
			end = OS64_AUDIO_BUSY;   // another thread is parked here, and the room it waits for is its
		if (end != 0)
		{
			spinlock_release_irqrestore(&s_lock, flags);
			return placed > 0 ? (long)placed : end;
		}

		long took = audio_stream_write(&s_stream, bytes + placed, length - placed, generation);
		if (took < 0)
		{
			// Flushed or failed: nothing this call placed survives, so the
			// count it had is not an answer.
			spinlock_release_irqrestore(&s_lock, flags);
			return took;
		}
		if (took > 0)
		{
			placed += (size_t)took;
			republish_locked();
			nudge_sink_locked();
		}
		if (placed == length)
		{
			spinlock_release_irqrestore(&s_lock, flags);
			return (long)placed;
		}
		if (deadline != 0 && kTicksSinceStart >= deadline)
		{
			spinlock_release_irqrestore(&s_lock, flags);
			return placed > 0 ? (long)placed : AUDIO_ERR_TIMEOUT;
		}

		// Full: register in the same hold as the write that found it so, so
		// a pull either made room before it (taken above) or will see us.
		s_writer = self;
		s_writer_parks++;
		parked_at = micros_now();
		spinlock_release_irqrestore(&s_lock, flags);   // never park holding the lock

		uint64_t wake = kTicksSinceStart + AUDIO_BACKSTOP_TICKS;
		if (deadline != 0 && deadline < wake)
			wake = deadline;
		signal_raise(SIGSLEEP, wake, self);
		// Woken by a pull, a flush, a close, a failed sink, the deadline, a
		// signal or the backstop: loop and look again.
	}
}

// ── The position and the tap ────────────────────────────────────────────────

long audio_position_read(audio_open_t *o, void *out, size_t capacity)
{
	if (o->face != AUDIO_FACE_POSITION)
		return AUDIO_ERR_CLOSED;
	os64_audio_position_t p;
	uint64_t flags = spinlock_acquire_irqsave(&s_lock);
	audio_stream_position(&s_stream, &p);
	spinlock_release_irqrestore(&s_lock, flags);
	if (capacity > sizeof(p))
		capacity = sizeof(p);
	memcpy(out, &p, capacity);
	return (long)capacity;
}

long audio_tap_read(audio_open_t *o, uint8_t *out, size_t capacity, uint64_t deadline)
{
	if (o->face != AUDIO_FACE_TAP || capacity < OS64_AUDIO_TAP_READ_MIN)
		return AUDIO_ERR_CLOSED;
	if (capacity > AUDIO_TAP_READ_MAX)
		capacity = AUDIO_TAP_READ_MAX;

	thread_t *self = get_core_local_storage()->currentThread;
	for (;;)
	{
		uint64_t flags = spinlock_acquire_irqsave(&s_lock);
		if (s_listener == self)
			s_listener = NULL;
		scheduler_forget_owed_wake(self);

		long end = 0;
		if (o->closed)
			end = AUDIO_ERR_CLOSED;
		else if (signal_park_must_end(self))
			end = AUDIO_ERR_INTERRUPTED;
		else if (s_listener != NULL)
			end = OS64_AUDIO_BUSY;
		else
		{
			size_t got = audio_stream_tap_read(&s_stream, out, capacity);
			if (got > 0)
				end = (long)got;
			else if (deadline != 0 && kTicksSinceStart >= deadline)
				end = AUDIO_ERR_TIMEOUT;
		}
		if (end != 0)
		{
			spinlock_release_irqrestore(&s_lock, flags);
			return end;
		}

		s_listener = self;
		spinlock_release_irqrestore(&s_lock, flags);

		uint64_t wake = kTicksSinceStart + AUDIO_BACKSTOP_TICKS;
		if (deadline != 0 && deadline < wake)
			wake = deadline;
		signal_raise(SIGSLEEP, wake, self);
	}
}

void audio_tap_undelivered(const uint8_t *records, size_t bytes)
{
	uint32_t count = 0;
	for (size_t at = 0; at + sizeof(os64_audio_tap_t) <= bytes; count++)
	{
		os64_audio_tap_t header;
		memcpy(&header, records + at, sizeof(header));
		at += sizeof(header) + (size_t)header.frames * OS64_AUDIO_FRAME_BYTES;
	}
	uint64_t flags = spinlock_acquire_irqsave(&s_lock);
	audio_stream_tap_lost(&s_stream, count);
	spinlock_release_irqrestore(&s_lock, flags);
}

// ── The sinks and the pull ──────────────────────────────────────────────────

// After a pull, s_lock held: the writer has room, the listener has records.
static void pulled_locked(void)
{
	republish_locked();
	if (audio_stream_writer_should_wake(&s_stream))
		wake_writer_locked();
	if (s_stream.tap_fill > 0)
		wake_listener_locked();
}

audio_pulled_t audio_pull(const audio_sink_t *self, int16_t *out, uint32_t frames)
{
	uint64_t flags = spinlock_acquire_irqsave(&s_lock);
	audio_pulled_t r = audio_stream_pull(&s_stream, self, out, frames, micros_now());
	pulled_locked();
	spinlock_release_irqrestore(&s_lock, flags);
	return r;
}

// A sink came, went or failed: every waiter looks again, and the sink that
// is now the sink is told to.
static void sink_changed_locked(void)
{
	republish_locked();
	wake_writer_locked();
	nudge_sink_locked();
}

void audio_sink_attach(const audio_sink_t *sink)
{
	uint64_t flags = spinlock_acquire_irqsave(&s_lock);
	audio_stream_sink_attach(&s_stream, sink);
	s_sink = sink;
	s_failed_why = NULL;
	sink_changed_locked();
	spinlock_release_irqrestore(&s_lock, flags);
}

void audio_sink_detach(const audio_sink_t *sink)
{
	uint64_t flags = spinlock_acquire_irqsave(&s_lock);
	if (s_sink == sink)
	{
		audio_stream_sink_detach(&s_stream, sink);
		s_sink = NULL;
		s_failed_why = NULL;
		sink_changed_locked();
	}
	spinlock_release_irqrestore(&s_lock, flags);
}

void audio_sink_failed(const audio_sink_t *sink, const char *why)
{
	uint64_t flags = spinlock_acquire_irqsave(&s_lock);
	if (s_sink == sink)
	{
		audio_stream_sink_failed(&s_stream, sink);
		s_failed_why = why;
		sink_changed_locked();
	}
	spinlock_release_irqrestore(&s_lock, flags);
}

void audio_sink_delay(const audio_sink_t *sink, uint32_t delay_us)
{
	uint64_t flags = spinlock_acquire_irqsave(&s_lock);
	audio_stream_sink_delay(&s_stream, sink, delay_us);
	spinlock_release_irqrestore(&s_lock, flags);
}

bool audio_sink_has_sound(const audio_sink_t *sink)
{
	uint64_t flags = spinlock_acquire_irqsave(&s_lock);
	bool sound = audio_stream_has_sound(&s_stream, sink);
	spinlock_release_irqrestore(&s_lock, flags);
	return sound;
}

// The null sink's buffer. kaudio's alone, and filled under s_lock.
static int16_t s_null_frames[AUDIO_RING_FRAMES * OS64_AUDIO_CHANNELS];

void audio_null_pump(void)
{
	uint64_t flags = spinlock_acquire_irqsave(&s_lock);
	if (kAudioClocked)
	{
		uint64_t now = micros_now();
		s_pump_wakes++;
		if (s_pump_last_us != 0)
		{
			uint64_t gap = now - s_pump_last_us;
			if (gap > s_pump_gap_max_us)
				s_pump_gap_max_us = gap;
			if (gap > s_pump_gap_max_since_read_us)
				s_pump_gap_max_since_read_us = gap;
		}
		s_pump_last_us = now;

		bool slip;
		uint32_t due = audio_clock_due(s_anchor_us, now, s_anchor_taken, s_stream.rate,
		                               s_stream.capacity, &slip);
		if (due > 0)
		{
			(void)audio_stream_pull(&s_stream, NULL, s_null_frames, due, now);
			s_anchor_taken += due;
		}
		if (slip)
		{
			// Held off for longer than the ring is: what was owed beyond
			// one ring is forgiven, and the clock counts from here.
			s_clock_slips++;
			s_anchor_us = now;
			s_anchor_taken = 0;
		}
		pulled_locked();
	}
	spinlock_release_irqrestore(&s_lock, flags);
}

// ── The controls ────────────────────────────────────────────────────────────

// One word of a line: skips blanks, copies up to the next blank or the end.
static size_t next_word(const char *text, size_t length, size_t *at, char *word, size_t capacity)
{
	size_t n = 0;
	while (*at < length && (text[*at] == ' ' || text[*at] == '\t' || text[*at] == '\n' || text[*at] == '\r'))
		(*at)++;
	while (*at < length && text[*at] != ' ' && text[*at] != '\t' && text[*at] != '\n' && text[*at] != '\r')
	{
		if (n + 1 < capacity)
			word[n++] = text[*at];
		(*at)++;
	}
	word[n] = '\0';
	return n;
}

bool audio_control(const char *text, size_t length)
{
	char verb[16], a[16], b[16], c[16], extra[4];
	size_t at = 0;
	next_word(text, length, &at, verb, sizeof(verb));

	if (strcmp(verb, "format") == 0)
	{
		// The one format there is. The verb exists so that asking for it is
		// already how a second one will be asked for.
		next_word(text, length, &at, a, sizeof(a));
		next_word(text, length, &at, b, sizeof(b));
		next_word(text, length, &at, c, sizeof(c));
		if (next_word(text, length, &at, extra, sizeof(extra)) != 0)
			return false;
		return strcmp(a, "s16le") == 0 && strcmp(b, "2") == 0 && strcmp(c, "48000") == 0;
	}
	if (next_word(text, length, &at, extra, sizeof(extra)) != 0)
		return false;

	bool done = false;
	uint64_t flags = spinlock_acquire_irqsave(&s_lock);
	if (strcmp(verb, "pause") == 0)
		done = audio_stream_pause(&s_stream);
	else if (strcmp(verb, "resume") == 0)
		done = audio_stream_resume(&s_stream);
	else if (strcmp(verb, "flush") == 0)
	{
		audio_stream_flush(&s_stream);
		wake_writer_locked();   // parked or between pieces, it answers FLUSHED
		done = true;
	}
	if (done)
	{
		republish_locked();
		nudge_sink_locked();
	}
	spinlock_release_irqrestore(&s_lock, flags);
	return done;
}

bool audio_volume_write(const char *text, size_t length)
{
	char word[8], extra[4];
	size_t at = 0;
	size_t n = next_word(text, length, &at, word, sizeof(word));
	if (n == 0 || n > 3 || next_word(text, length, &at, extra, sizeof(extra)) != 0)
		return false;
	uint32_t volume = 0;
	for (size_t i = 0; i < n; i++)
	{
		if (word[i] < '0' || word[i] > '9')
			return false;
		volume = volume * 10 + (uint32_t)(word[i] - '0');
	}
	uint64_t flags = spinlock_acquire_irqsave(&s_lock);
	bool done = audio_stream_set_volume(&s_stream, volume);
	spinlock_release_irqrestore(&s_lock, flags);
	return done;
}

static const char *state_name(uint32_t state)
{
	switch (state)
	{
		case OS64_AUDIO_IDLE:         return "idle";
		case OS64_AUDIO_WAITING:      return "waiting";
		case OS64_AUDIO_PLAYING:      return "playing";
		case OS64_AUDIO_STARVED:      return "starved";
		case OS64_AUDIO_PAUSED:       return "paused";
		case OS64_AUDIO_DRAINING:     return "draining";
		case OS64_AUDIO_FAILED_STATE: return "failed";
		default:                      return "?";
	}
}

size_t audio_control_text(char *out, size_t capacity)
{
	uint64_t flags = spinlock_acquire_irqsave(&s_lock);
	uint32_t state = audio_stream_state(&s_stream);
	uint32_t generation = s_stream.generation;
	uint32_t rate = s_stream.rate;
	spinlock_release_irqrestore(&s_lock, flags);
	int n = snprintf(out, capacity, "format s16le %u %u\nstate %s\ngeneration %u\n",
	                 OS64_AUDIO_CHANNELS, rate, state_name(state), generation);
	return n < 0 ? 0 : ((size_t)n < capacity ? (size_t)n : capacity - 1);
}

size_t audio_volume_text(char *out, size_t capacity)
{
	int n = snprintf(out, capacity, "%u\n", s_stream.volume);
	return n < 0 ? 0 : ((size_t)n < capacity ? (size_t)n : capacity - 1);
}

size_t audio_report(char *out, size_t capacity)
{
	uint64_t flags = spinlock_acquire_irqsave(&s_lock);
	os64_audio_position_t p;
	audio_stream_position(&s_stream, &p);
	audio_stream_t s = s_stream;   // the counters, as of the same instant
	// Copied while the lock holds the sink in place: a sink may detach, and
	// take its strings with it, the moment the lock is released.
	char sink[32], why[64];
	strncpy(sink, s_sink != NULL ? s_sink->name : "null", sizeof(sink) - 1);
	sink[sizeof(sink) - 1] = '\0';
	strncpy(why, s_failed_why != NULL ? s_failed_why : "no reason given", sizeof(why) - 1);
	why[sizeof(why) - 1] = '\0';
	char owner[sizeof(s_owner)];
	memcpy(owner, s_owner, sizeof(owner));
	uint64_t slips = s_clock_slips, parks = s_writer_parks, wait_max = s_writer_wait_max_us;
	uint64_t wakes = s_pump_wakes, gap_max = s_pump_gap_max_us;
	uint64_t gap_since = s_pump_gap_max_since_read_us;
	uint64_t underruns_since = s.total_underruns - s_underruns_at_read;
	bool clocked = kAudioClocked;
	bool tap = s_tap_open != NULL;
	s_underruns_at_read = s.total_underruns;
	s_pump_gap_max_since_read_us = 0;
	spinlock_release_irqrestore(&s_lock, flags);

	int n = snprintf(out, capacity,
		"sink: %s%s%s\n"
		"format: s16le %u %u\n"
		"state: %s\n"
		"generation: %u\n"
		"opened_by: %s\n"
		"volume: %u\n"
		"ring_frames: %u of %u\n"
		"ring_ms: %u of %u\n"
		"accepted: %lu\n"
		"taken: %lu\n"
		"heard: %lu\n"
		"position_micros: %lu\n"
		"delay_us: %u\n"
		"underruns: %lu\n"
		"underruns_since_boot: %lu\n"
		"underruns_since_read: %lu\n"
		"frames_given: %lu\n"
		"silence_given: %lu\n"
		"pulls_refused: %lu\n"
		"writer_parks: %lu\n"
		"writer_wait_max_us: %lu\n"
		"null_sink_clocked: %s\n"
		"kaudio_wakes: %lu\n"
		"kaudio_gap_max_us: %lu\n"
		"kaudio_gap_max_since_read_us: %lu\n"
		"clock_slips: %lu\n"
		"tap: %s\n"
		"tap_dropped: %lu\n",
		sink, p.state == OS64_AUDIO_FAILED_STATE ? " FAILED: " : "",
		p.state == OS64_AUDIO_FAILED_STATE ? why : "",
		OS64_AUDIO_CHANNELS, p.rate,
		state_name(p.state),
		p.generation,
		owner[0] != '\0' ? owner : "-",
		s.volume,
		p.queued, s.capacity,
		p.queued * 1000u / p.rate, s.capacity * 1000u / p.rate,
		p.accepted, p.taken, p.heard, p.micros, p.delay_us,
		p.underruns, s.total_underruns, underruns_since,
		s.total_given, s.total_silence, s.refused_pulls,
		parks, wait_max,
		clocked ? "yes" : "no",
		wakes, gap_max, gap_since, slips,
		tap ? "open" : "closed",
		s.tap_dropped_total);
	return n < 0 ? 0 : ((size_t)n < capacity ? (size_t)n : capacity - 1);
}
