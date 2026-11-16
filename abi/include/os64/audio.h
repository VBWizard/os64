#ifndef OS64_ABI_AUDIO_H
#define OS64_ABI_AUDIO_H

// os64/audio.h — sound as a file (docs/design/pending/AUDIO_SEAM.md).
//
//   /dev/audio     write PCM: signed 16-bit little-endian, two channels,
//                  48,000 frames a second. A FRAME is one sample for each
//                  channel, four bytes. One open at a time. The write waits
//                  for room under write's patience (os64_write_for): forever,
//                  some milliseconds, or not at all.
//   /dev/audiopos  each read returns one os64_audio_position_t.
//   /dev/audiotap  each read returns whole records of what the sink was
//                  given: an os64_audio_tap_t and its frames. One open at a
//                  time. The buffer must hold OS64_AUDIO_TAP_READ_MIN bytes.
//   /dev/audioctl  text. Write `pause`, `resume`, `flush`, or
//                  `format s16le 2 48000`; read the state back.
//   /dev/volume    text, 0 to 100.
//
// Closing /dev/audio never waits and never cuts the sound: what was queued
// plays out, and the next open's sound follows it. `flush` is how to stop
// it at once.

#include <stdint.h>

#define OS64_AUDIO_RATE         48000u
#define OS64_AUDIO_CHANNELS     2u
#define OS64_AUDIO_FRAME_BYTES  4u

// Negative answers of this family. -1 (refused), -2 (unreadable memory),
// OS64_INTERRUPTED (-4) and OS64_ERR_TIMEOUT (-10) keep their system-wide
// meanings here.
#define OS64_AUDIO_BUSY     (-20)   // open: already open. write: another thread is waiting in one
#define OS64_AUDIO_FLUSHED  (-21)   // write: a flush retired this call's sound, all of it
#define OS64_AUDIO_FAILED   (-22)   // write: the sink failed; /sys/audio says why

// The stream's state (os64_audio_position_t.state).
#define OS64_AUDIO_IDLE      0u   // nothing open, nothing queued
#define OS64_AUDIO_WAITING   1u   // open, nothing written yet
#define OS64_AUDIO_PLAYING   2u
#define OS64_AUDIO_STARVED   3u   // open and written, and the ring ran dry
#define OS64_AUDIO_PAUSED    4u
#define OS64_AUDIO_DRAINING  5u   // closed; what was queued is playing out
#define OS64_AUDIO_FAILED_STATE 6u

// The playback position. Counts are in the PROGRAM's own frames: `accepted`
// is how many it has had taken from it this generation, so frame N of the
// record is frame N of what it wrote, and silence the system inserts is
// never counted.
//
// Between readings, and only in state PLAYING, a player extrapolates:
//   heard + (now - micros) * rate / 1000000, never past `taken`
// with `now` from os64_micros(). In every other state it uses `heard` as
// read. A reading whose `generation` is not the one the player is feeding
// describes another stream and is discarded.
typedef struct os64_audio_position
{
	uint32_t size;         // bytes of this record the system filled
	uint32_t generation;   // goes up when the counts below start again: a flush, an open, a change of sink
	uint32_t state;        // OS64_AUDIO_*
	uint32_t rate;         // frames a second
	uint64_t accepted;     // frames taken from the program
	uint64_t taken;        // of those, frames the sink has pulled. Exact
	uint64_t heard;        // of those, an ESTIMATE of how many have reached the ear: taken less delay_us
	uint64_t micros;       // the clock at which taken and heard were true
	uint32_t queued;       // frames waiting ahead of the next one written (an earlier owner's too)
	uint32_t delay_us;     // what the sink says lies beyond its pull
	uint64_t underruns;    // times the ring ran dry while playing
} os64_audio_position_t;

_Static_assert(sizeof(os64_audio_position_t) == 64, "os64_audio_position_t is 64 bytes");

// One record of the tap: a header, then `frames` frames exactly as the sink
// received them (the volume applied). A record is one generation's, and is
// all sound or all silence.
#define OS64_AUDIO_TAP_RECORD_FRAMES 1024u
typedef struct os64_audio_tap
{
	uint32_t generation;
	uint32_t frames;       // how many follow this header
	uint64_t first;        // index of the first, counting every frame given to a sink since boot
	uint64_t micros;       // when the pull happened
	uint32_t silence;      // 1: fill, the ring had nothing
	uint32_t dropped;      // records lost before this one (a full capture ring)
} os64_audio_tap_t;

_Static_assert(sizeof(os64_audio_tap_t) == 32, "os64_audio_tap_t is 32 bytes");

#define OS64_AUDIO_TAP_READ_MIN \
	(sizeof(os64_audio_tap_t) + OS64_AUDIO_TAP_RECORD_FRAMES * OS64_AUDIO_FRAME_BYTES)

#endif // OS64_ABI_AUDIO_H
