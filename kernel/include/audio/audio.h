// audio.h — sound as a file: the kernel half of /dev/audio and its
// neighbours (docs/design/pending/AUDIO_SEAM.md; os64/audio.h is the ABI).
//
// One stream in the machine. A program writes PCM into its ring; a SINK
// empties the ring by pulling, from its own thread at its own pace. With no
// sink attached the NULL SINK is the sink: kaudio (below) takes frames at
// the rate a real output would and discards them.
//
// LOCKING: one spinlock, interrupts off, around the stream and everything
// that describes it. A wake is made under it, so it is taken BEFORE the
// scheduler's queue lock and never under it: the tick, which runs inside the
// scheduler's pass, reads kAudioClocked and takes nothing.

#ifndef AUDIO_H
#define AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "doorbell.h"
#include "task.h"
#include "audio/audio_stream.h"

// ── The handle (HANDLE_AUDIO) ───────────────────────────────────────────────
// One tag, three faces. An open is counted two ways: HANDLES that name it
// (the last one's close is the close) and holders of its memory (each
// handle, and each operation in flight; handle.c § The pin).

typedef enum
{
	AUDIO_FACE_STREAM,     // /dev/audio: write PCM
	AUDIO_FACE_POSITION,   // /dev/audiopos: read the position record
	AUDIO_FACE_TAP         // /dev/audiotap: read what the sink was given
} audio_face_t;

typedef struct audio_open audio_open_t;

// A new open holding one handle. NULL with *refusal set: OS64_AUDIO_BUSY
// (the stream and the tap are one open at a time) or -1 (no memory).
audio_open_t *audio_open(audio_face_t face, long *refusal);
audio_face_t audio_open_face(const audio_open_t *o);
void audio_open_ref(audio_open_t *o);
void audio_open_release(audio_open_t *o);
// One more handle on a stream open, for a child it is shared into. False
// for the other faces, which have one holder by design.
bool audio_open_share(audio_open_t *o);
// A handle goes. The last one marks the open closed, wakes an operation
// parked in it, and for the stream leaves what is queued to play out. Never
// waits.
void audio_open_close(audio_open_t *o);

// Results other than a count. The OS64_AUDIO_* codes pass through as they
// are.
#define AUDIO_ERR_CLOSED       (-1)    // the handle was closed under the call, or the face has no such verb
#define AUDIO_ERR_INTERRUPTED  (-3)    // signal_park_must_end
#define AUDIO_ERR_TIMEOUT      (-5)    // the deadline passed

// The generation a write begins under; every piece of it is offered with
// this number, so a flush between two pieces retires the call.
uint32_t audio_write_begin(audio_open_t *o);
// Place `length` bytes of kernel memory, waiting for room until `deadline`
// (a tick; 0 = no limit). Returns `length`; or fewer when the wait ended
// first (a deadline, a signal, a close) with some placed; or an error with
// none placed; or OS64_AUDIO_FLUSHED / _FAILED whatever was placed. Task
// context only.
long audio_write(audio_open_t *o, const uint8_t *bytes, size_t length,
                 uint32_t generation, uint64_t deadline);
// The position face: up to `capacity` bytes of the record. Never waits.
long audio_position_read(audio_open_t *o, void *out, size_t capacity);
// The tap face: whole records, waiting for one until `deadline`. A buffer
// smaller than OS64_AUDIO_TAP_READ_MIN is refused (AUDIO_ERR_CLOSED).
long audio_tap_read(audio_open_t *o, uint8_t *out, size_t capacity, uint64_t deadline);
// Records a read took and could not hand to its caller.
void audio_tap_undelivered(const uint8_t *records, size_t bytes);

// ── The controls (/dev/audioctl, /dev/volume) ───────────────────────────────
// One line of text each; false is "refused". Any context that may take a
// spinlock; never waits.
bool audio_control(const char *text, size_t length);
bool audio_volume_write(const char *text, size_t length);
size_t audio_control_text(char *out, size_t capacity);
size_t audio_volume_text(char *out, size_t capacity);
// /sys/audio. The since-read counters start again at each call.
size_t audio_report(char *out, size_t capacity);

// ── The sink seam ───────────────────────────────────────────────────────────
// The seam never calls into a sink with a fact: a pause and a flush are
// what the sink's next pull tells it (audio_pulled_t). `nudge` only says
// "pull soon".

typedef struct audio_sink
{
	const char *name;             // for /sys/audio
	void (*nudge)(void *ctx);     // optional. Called with the stream's lock
	                              // held and interrupts off: set a flag or
	                              // ring a doorbell, nothing more
	void *ctx;
} audio_sink_t;

void audio_sink_attach(const audio_sink_t *sink);
void audio_sink_detach(const audio_sink_t *sink);
void audio_sink_failed(const audio_sink_t *sink, const char *why);   // `why` and the sink's name must last until it detaches
void audio_sink_delay(const audio_sink_t *sink, uint32_t delay_us);
// Has `sink` sound to take now: it is the sink, the ring holds frames and
// the stream is not paused (audio_stream_has_sound). Any thread; never
// sleeps; takes the stream's lock, so never from inside a nudge.
bool audio_sink_has_sound(const audio_sink_t *sink);
// audio_sink_has_sound's answer and, in *paused, whether the stream is
// paused (whoever the sink is), read in one hold of the lock so the two
// agree. The same rules as audio_sink_has_sound.
bool audio_sink_sound(const audio_sink_t *sink, bool *paused);
// Fill `out` with `frames` frames (audio_stream_pull) and wake whoever was
// waiting on what it changed. Any thread; never sleeps.
audio_pulled_t audio_pull(const audio_sink_t *self, int16_t *out, uint32_t frames);

// ── The null sink's pump ────────────────────────────────────────────────────

extern doorbell_t kAudioDoorbell;
extern task_t    *kKaudioTask;
// The null sink is the sink and the stream has work. Written under the
// stream's lock; the tick reads it bare to decide whether to ring kaudio.
extern volatile bool kAudioClocked;

void audio_init(void);         // the ring; before anything can open /dev/audio
void kaudio_thread(void);      // task.c gives the "/kaudio" builtin this body
void audio_null_pump(void);    // one wake of kaudio: take what the clock says is due

#endif // AUDIO_H
