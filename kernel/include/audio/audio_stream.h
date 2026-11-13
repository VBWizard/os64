// audio_stream.h — the one stream of sound, as arithmetic
// (docs/design/pending/AUDIO_SEAM.md).
//
// PURE: no kernel headers, no lock, no clock of its own. The caller owns the
// lock around every call, hands in the storage, and passes the time.
// tools/test_audio_stream_host.sh runs it beside a plain model;
// tools/test_audio_stream_mutants.py breaks each rule.

#ifndef AUDIO_STREAM_H
#define AUDIO_STREAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "os64/audio.h"

// The ring, sized in time: 200 ms.
#define AUDIO_RING_FRAMES  (OS64_AUDIO_RATE / 5u)
// The capture ring behind /dev/audiotap: about a second, with its headers.
#define AUDIO_TAP_BYTES    (OS64_AUDIO_RATE * OS64_AUDIO_FRAME_BYTES + 8192u)

typedef struct audio_stream
{
	// The ring: `fill` frames starting at `head`. The first `old_frames` of
	// them were queued by an earlier open and belong to `old_generation`.
	int16_t *ring;
	uint32_t capacity;
	uint32_t head;
	uint32_t fill;
	uint32_t old_frames;
	uint32_t old_generation;

	// Bytes of a frame a write ended inside.
	uint8_t  split[OS64_AUDIO_FRAME_BYTES];
	uint32_t split_len;

	// The facts the state is derived from.
	bool open;          // a /dev/audio handle exists
	bool paused;
	bool written;       // this generation has had a frame accepted
	bool failed;        // the sink failed closed
	bool dry_counted;   // the dry spell under way has its underrun

	uint32_t generation;
	uint32_t rate;
	uint64_t accepted;
	uint64_t taken;
	uint64_t taken_micros;
	uint64_t underruns;
	uint32_t delay_us;

	// Who may pull. NULL is the null sink.
	const void *sink;

	uint32_t volume;     // 0..100
	int32_t  gain_q15;   // (volume/100)^2

	// Since boot.
	uint64_t total_given;       // every frame a pull filled, sound and silence
	uint64_t total_silence;
	uint64_t total_underruns;
	uint64_t refused_pulls;     // pulls by a sink that was not the sink

	// The tap: a byte ring of whole records. NULL while nobody listens.
	uint8_t *tap;
	uint32_t tap_capacity;
	uint32_t tap_head;
	uint32_t tap_fill;
	uint32_t tap_dropped;        // since the last record that got in
	uint64_t tap_dropped_total;
} audio_stream_t;

void audio_stream_init(audio_stream_t *s, int16_t *ring, uint32_t capacity_frames, uint32_t rate);

// OS64_AUDIO_*.
uint32_t audio_stream_state(const audio_stream_t *s);

// The /dev/audio handle. Open is refused (false) while one exists. The close
// is the last handle's.
bool audio_stream_open(audio_stream_t *s);
void audio_stream_close(audio_stream_t *s);

// Place bytes. `generation` is the one the caller's write began under. Returns
// how many bytes were taken (a split frame's included), which is less than
// `length` only when the ring filled; or OS64_AUDIO_FLUSHED / _FAILED.
long audio_stream_write(audio_stream_t *s, const uint8_t *bytes, size_t length, uint32_t generation);

// What a pull tells its sink.
typedef struct audio_pulled
{
	uint32_t frames;       // how many of `out` are the ring's; the rest is silence
	uint32_t generation;   // the generation those frames belong to
	bool     paused;       // the stream is paused: `out` is untouched
	bool     yours;        // false: another sink is the sink. `out` is untouched
} audio_pulled_t;

// Fill `out` with `frames` frames: the ring's (one generation's, the gain
// applied), then silence. Stamps the position with `now_us`.
audio_pulled_t audio_stream_pull(audio_stream_t *s, const void *sink, int16_t *out,
                                 uint32_t frames, uint64_t now_us);

// The control verbs. False is "refused": nothing to pause, not paused.
bool audio_stream_pause(audio_stream_t *s);
bool audio_stream_resume(audio_stream_t *s);
void audio_stream_flush(audio_stream_t *s);
bool audio_stream_set_volume(audio_stream_t *s, uint32_t volume);

// A change of sink is a flush that always moves the generation. Detach and
// failed are ignored from a sink that is not the sink.
void audio_stream_sink_attach(audio_stream_t *s, const void *sink);
void audio_stream_sink_detach(audio_stream_t *s, const void *sink);
void audio_stream_sink_failed(audio_stream_t *s, const void *sink);
void audio_stream_sink_delay(audio_stream_t *s, const void *sink, uint32_t delay_us);

void audio_stream_position(const audio_stream_t *s, os64_audio_position_t *out);

// Is a parked writer worth waking for room: a quarter of the ring is free.
// (A flush, a close and a failed sink wake it whatever this says.)
bool audio_stream_writer_should_wake(const audio_stream_t *s);
// Has the null sink's clock anything to run for.
bool audio_stream_has_work(const audio_stream_t *s);

// The null sink's clock: frames due at `now_us` for a sink anchored at
// `anchor_us` that has taken `taken` since. More than `capacity` due is a
// slip: `capacity` is answered and *slip set, and the caller re-anchors.
uint32_t audio_clock_due(uint64_t anchor_us, uint64_t now_us, uint64_t taken,
                         uint32_t rate, uint32_t capacity, bool *slip);

// The tap. Open hands in storage (false: already open); close gives it back.
bool audio_stream_tap_open(audio_stream_t *s, uint8_t *storage, uint32_t capacity);
uint8_t *audio_stream_tap_close(audio_stream_t *s);
// Whole records into `out`, as many as fit; 0 when there are none.
size_t audio_stream_tap_read(audio_stream_t *s, uint8_t *out, size_t capacity);
// Records taken by a read that could not be delivered.
void audio_stream_tap_lost(audio_stream_t *s, uint32_t records);

#endif // AUDIO_STREAM_H
