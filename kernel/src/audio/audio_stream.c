// audio_stream.c — the one stream of sound, as arithmetic. audio_stream.h is
// the contract and AUDIO_SEAM.md the design; nothing here knows a lock, a
// thread or a clock.

#include "audio/audio_stream.h"

#define GAIN_UNITY 32768

void audio_stream_init(audio_stream_t *s, int16_t *ring, uint32_t capacity_frames, uint32_t rate)
{
	uint8_t *raw = (uint8_t *)s;
	for (size_t i = 0; i < sizeof(*s); i++)
		raw[i] = 0;
	s->ring = ring;
	s->capacity = capacity_frames;
	s->rate = rate;
	s->volume = 100;
	s->gain_q15 = GAIN_UNITY;
}

uint32_t audio_stream_state(const audio_stream_t *s)
{
	if (s->failed)
		return OS64_AUDIO_FAILED_STATE;
	if (!s->open)
		return s->fill > 0 ? OS64_AUDIO_DRAINING : OS64_AUDIO_IDLE;
	if (s->paused)
		return OS64_AUDIO_PAUSED;
	if (s->fill > 0)
		return OS64_AUDIO_PLAYING;
	return s->written ? OS64_AUDIO_STARVED : OS64_AUDIO_WAITING;
}

// Empty the ring and start the counts again under a new generation.
static void retire_everything(audio_stream_t *s)
{
	s->head = 0;
	s->fill = 0;
	s->old_frames = 0;
	s->split_len = 0;
	s->generation++;
	s->accepted = 0;
	s->taken = 0;
	s->underruns = 0;
	s->written = false;
	s->dry_counted = false;
}

bool audio_stream_open(audio_stream_t *s)
{
	if (s->open)
		return false;
	s->open = true;
	// What an earlier open left in the ring plays first and stays that
	// generation's. (Two earlier opens' frames are both counted as the later
	// one's: the tap is the only reader of the number.)
	s->old_generation = s->generation;
	s->old_frames = s->fill;
	s->generation++;
	s->accepted = 0;
	s->taken = 0;
	s->underruns = 0;
	s->written = false;
	s->dry_counted = false;
	s->split_len = 0;
	return true;
}

void audio_stream_close(audio_stream_t *s)
{
	if (!s->open)
		return;
	// A pause belongs to an open: sound nobody is left to resume is not kept.
	if (s->paused)
	{
		retire_everything(s);
		s->paused = false;
	}
	s->open = false;
	s->split_len = 0;
}

static void ring_put(audio_stream_t *s, const uint8_t *frame)
{
	uint32_t at = s->head + s->fill;
	if (at >= s->capacity)
		at -= s->capacity;
	s->ring[at * 2]     = (int16_t)(uint16_t)(frame[0] | (frame[1] << 8));
	s->ring[at * 2 + 1] = (int16_t)(uint16_t)(frame[2] | (frame[3] << 8));
	s->fill++;
	s->accepted++;
	s->written = true;
	s->dry_counted = false;
}

long audio_stream_write(audio_stream_t *s, const uint8_t *bytes, size_t length, uint32_t generation)
{
	if (s->failed)
		return OS64_AUDIO_FAILED;
	if (generation != s->generation)
		return OS64_AUDIO_FLUSHED;

	size_t used = 0;

	// A frame an earlier write ended inside is finished first. The bytes
	// that would finish it are not taken while the ring has no room for it:
	// a byte this call counts is a byte that will be played.
	if (s->split_len > 0)
	{
		if (s->split_len + length >= OS64_AUDIO_FRAME_BYTES && s->fill == s->capacity)
			return 0;
		while (s->split_len < OS64_AUDIO_FRAME_BYTES && used < length)
			s->split[s->split_len++] = bytes[used++];
		if (s->split_len < OS64_AUDIO_FRAME_BYTES)
			return (long)used;
		ring_put(s, s->split);
		s->split_len = 0;
	}

	while (length - used >= OS64_AUDIO_FRAME_BYTES)
	{
		if (s->fill == s->capacity)
			return (long)used;
		ring_put(s, bytes + used);
		used += OS64_AUDIO_FRAME_BYTES;
	}

	while (used < length)
		s->split[s->split_len++] = bytes[used++];
	return (long)used;
}

// ── The tap ─────────────────────────────────────────────────────────────────

static void tap_put(audio_stream_t *s, const uint8_t *bytes, uint32_t length)
{
	uint32_t at = s->tap_head + s->tap_fill;
	if (at >= s->tap_capacity)
		at -= s->tap_capacity;
	for (uint32_t i = 0; i < length; i++)
	{
		s->tap[at] = bytes[i];
		if (++at == s->tap_capacity)
			at = 0;
	}
	s->tap_fill += length;
}

// One record: `frames` frames at `pcm`, at most OS64_AUDIO_TAP_RECORD_FRAMES.
// Whole or not at all.
static void tap_record(audio_stream_t *s, uint32_t generation, uint64_t first, uint64_t now_us,
                       bool silence, const int16_t *pcm, uint32_t frames)
{
	uint32_t bytes = (uint32_t)sizeof(os64_audio_tap_t) + frames * OS64_AUDIO_FRAME_BYTES;
	if (bytes > s->tap_capacity - s->tap_fill)
	{
		s->tap_dropped++;
		s->tap_dropped_total++;
		return;
	}
	os64_audio_tap_t header;
	header.generation = generation;
	header.frames = frames;
	header.first = first;
	header.micros = now_us;
	header.silence = silence ? 1 : 0;
	header.dropped = s->tap_dropped;
	s->tap_dropped = 0;
	tap_put(s, (const uint8_t *)&header, (uint32_t)sizeof(header));
	tap_put(s, (const uint8_t *)pcm, frames * OS64_AUDIO_FRAME_BYTES);
}

static void tap_span(audio_stream_t *s, uint32_t generation, uint64_t first, uint64_t now_us,
                     bool silence, const int16_t *pcm, uint32_t frames)
{
	while (frames > 0)
	{
		uint32_t piece = frames > OS64_AUDIO_TAP_RECORD_FRAMES ? OS64_AUDIO_TAP_RECORD_FRAMES : frames;
		tap_record(s, generation, first, now_us, silence, pcm, piece);
		first += piece;
		pcm += piece * 2;
		frames -= piece;
	}
}

bool audio_stream_tap_open(audio_stream_t *s, uint8_t *storage, uint32_t capacity)
{
	if (s->tap != NULL)
		return false;
	s->tap = storage;
	s->tap_capacity = capacity;
	s->tap_head = 0;
	s->tap_fill = 0;
	s->tap_dropped = 0;
	return true;
}

uint8_t *audio_stream_tap_close(audio_stream_t *s)
{
	uint8_t *storage = s->tap;
	s->tap = NULL;
	s->tap_capacity = 0;
	s->tap_head = 0;
	s->tap_fill = 0;
	return storage;
}

static void tap_peek(const audio_stream_t *s, uint32_t offset, uint8_t *out, uint32_t length)
{
	uint32_t at = s->tap_head + offset;
	if (at >= s->tap_capacity)
		at -= s->tap_capacity;
	for (uint32_t i = 0; i < length; i++)
	{
		out[i] = s->tap[at];
		if (++at == s->tap_capacity)
			at = 0;
	}
}

size_t audio_stream_tap_read(audio_stream_t *s, uint8_t *out, size_t capacity)
{
	size_t done = 0;
	while (s->tap != NULL && s->tap_fill > 0)
	{
		os64_audio_tap_t header;
		tap_peek(s, 0, (uint8_t *)&header, (uint32_t)sizeof(header));
		uint32_t bytes = (uint32_t)sizeof(header) + header.frames * OS64_AUDIO_FRAME_BYTES;
		if (bytes > capacity - done)
			break;
		tap_peek(s, 0, out + done, bytes);
		done += bytes;
		s->tap_head += bytes;
		if (s->tap_head >= s->tap_capacity)
			s->tap_head -= s->tap_capacity;
		s->tap_fill -= bytes;
	}
	return done;
}

void audio_stream_tap_lost(audio_stream_t *s, uint32_t records)
{
	s->tap_dropped += records;
	s->tap_dropped_total += records;
}

// ── The pull ────────────────────────────────────────────────────────────────

static int16_t apply_gain(int16_t sample, int32_t gain_q15)
{
	if (gain_q15 == GAIN_UNITY)
		return sample;
	// Toward zero for both signs: a shift would round negatives away from it.
	return (int16_t)(((int32_t)sample * gain_q15) / GAIN_UNITY);
}

audio_pulled_t audio_stream_pull(audio_stream_t *s, const void *sink, int16_t *out,
                                 uint32_t frames, uint64_t now_us)
{
	audio_pulled_t r = { 0, s->generation, false, true };

	if (sink != s->sink)
	{
		s->refused_pulls++;
		r.yours = false;
		return r;
	}
	if (s->paused)
	{
		r.paused = true;
		return r;
	}

	// One generation's frames a pull: an earlier owner's first, and the pull
	// that ends them is silence from there on.
	uint32_t n;
	if (s->old_frames > 0)
	{
		n = frames < s->old_frames ? frames : s->old_frames;
		s->old_frames -= n;
		r.generation = s->old_generation;
	}
	else
	{
		n = frames < s->fill ? frames : s->fill;
		s->taken += n;
	}

	for (uint32_t i = 0; i < n; i++)
	{
		out[i * 2]     = apply_gain(s->ring[s->head * 2], s->gain_q15);
		out[i * 2 + 1] = apply_gain(s->ring[s->head * 2 + 1], s->gain_q15);
		if (++s->head == s->capacity)
			s->head = 0;
	}
	s->fill -= n;
	for (uint32_t i = n; i < frames; i++)
	{
		out[i * 2] = 0;
		out[i * 2 + 1] = 0;
	}
	r.frames = n;
	s->taken_micros = now_us;

	if (s->tap != NULL)
	{
		tap_span(s, r.generation, s->total_given, now_us, false, out, n);
		tap_span(s, s->generation, s->total_given + n, now_us, true, out + n * 2, frames - n);
	}
	s->total_given += frames;
	s->total_silence += frames - n;

	// An underrun is a pull that came up short of an open stream that has
	// been written to, counted once until a write ends the dry spell.
	if (n < frames && s->fill == 0 && s->open && s->written && !s->failed && !s->dry_counted)
	{
		s->dry_counted = true;
		s->underruns++;
		s->total_underruns++;
	}
	return r;
}

// ── The verbs ───────────────────────────────────────────────────────────────

bool audio_stream_pause(audio_stream_t *s)
{
	if (!s->open || s->paused || s->failed)
		return false;
	s->paused = true;
	return true;
}

bool audio_stream_resume(audio_stream_t *s)
{
	if (!s->paused)
		return false;
	s->paused = false;
	return true;
}

void audio_stream_flush(audio_stream_t *s)
{
	if (!s->open && s->fill == 0)
		return;   // nothing queued and nobody to tell
	retire_everything(s);
}

bool audio_stream_set_volume(audio_stream_t *s, uint32_t volume)
{
	if (volume > 100)
		return false;
	s->volume = volume;
	s->gain_q15 = (int32_t)(volume * volume * (uint32_t)GAIN_UNITY / 10000u);
	return true;
}

void audio_stream_sink_attach(audio_stream_t *s, const void *sink)
{
	s->sink = sink;
	s->failed = false;
	s->delay_us = 0;
	retire_everything(s);
}

void audio_stream_sink_detach(audio_stream_t *s, const void *sink)
{
	if (s->sink != sink)
		return;
	audio_stream_sink_attach(s, NULL);
}

void audio_stream_sink_failed(audio_stream_t *s, const void *sink)
{
	if (s->sink != sink)
		return;
	s->failed = true;
	s->head = 0;
	s->fill = 0;
	s->old_frames = 0;
	s->split_len = 0;
}

void audio_stream_sink_delay(audio_stream_t *s, const void *sink, uint32_t delay_us)
{
	if (s->sink == sink)
		s->delay_us = delay_us;
}

void audio_stream_position(const audio_stream_t *s, os64_audio_position_t *out)
{
	uint64_t behind = (uint64_t)s->delay_us * s->rate / 1000000u;
	out->size = (uint32_t)sizeof(*out);
	out->generation = s->generation;
	out->state = audio_stream_state(s);
	out->rate = s->rate;
	out->accepted = s->accepted;
	out->taken = s->taken;
	out->heard = s->taken > behind ? s->taken - behind : 0;
	out->micros = s->taken_micros;
	out->queued = s->fill;
	out->delay_us = s->delay_us;
	out->underruns = s->underruns;
}

bool audio_stream_writer_should_wake(const audio_stream_t *s)
{
	return s->capacity - s->fill >= s->capacity / 4;
}

bool audio_stream_has_work(const audio_stream_t *s)
{
	return !s->failed && !s->paused && (s->open || s->fill > 0);
}

uint32_t audio_clock_due(uint64_t anchor_us, uint64_t now_us, uint64_t taken,
                         uint32_t rate, uint32_t capacity, bool *slip)
{
	*slip = false;
	if (now_us <= anchor_us)
		return 0;
	uint64_t owed = (now_us - anchor_us) * rate / 1000000u;
	if (owed <= taken)
		return 0;
	uint64_t due = owed - taken;
	if (due > capacity)
	{
		*slip = true;
		return capacity;
	}
	return (uint32_t)due;
}
