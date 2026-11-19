#ifndef OS64_MEDIA_OUTPUT_H
#define OS64_MEDIA_OUTPUT_H
// os64/media_output.h — the sound device, shared by every sound a program
// plays (libmedia.so).
//
// THE SOUND NEVER WAITS ON THE CALLER'S THREAD. One audio thread per
// program owns /dev/audio, /dev/audiopos and /dev/audioctl and mixes every
// VOICE added to it into the one stream; nothing that keeps a voice
// sounding, starts it or recovers it needs the program to answer a bell.
//
// A voice is a sound some owner plays: a struct the owner keeps, adds, and
// removes. The thread, for every voice:
// - takes from the voice's queue and mixes it at the voice's own gain,
//   recording what it wrote at which stream frame (os64/media_mix.h's
//   anchors), so each voice's heard position is its own;
// - writes nothing of a voice until the voice `runs` (an owner with a
//   clock says so when its clock plays, so a picture's run-in after a seek
//   is never heard against a held picture);
// - tells the voice's clock (os64/media_clock.h), when it has one, what
//   the ear heard: only when the ear has moved (a PLAYING stream whose ear
//   stands still says nothing, so the clock's trust decides) or the stream
//   has stopped; and with no clock, keeps `position` itself;
// - drops, from a voice with a clock that is not being heard (it does not
//   want sound, or the device is refused), the frames its clock has passed,
//   and from a voice with a clock that writes again after a gap in the
//   device, what its clock passed meanwhile, so time keeps moving and its
//   queue keeps draining;
// - takes a stream generation it did not start (a sink attached or
//   detached, a flush from elsewhere) as a flush: what the device held is
//   gone, each voice carries on from its queue;
// - on a voice's own flush (its queue's generation moved), flushes the
//   device when that voice alone has sound in it, and otherwise lets the
//   old tail play out under the others;
// - holds the device while some voice wants sound, pauses it while nobody
//   plays and gives it back OS64_MEDIA_OUTPUT_PAUSE_KEEP_MS later, and
//   closes it OS64_MEDIA_OUTPUT_IDLE_CLOSE_MS after the last voice goes;
// - waits at most OS64_MEDIA_OUTPUT_WRITE_PATIENCE_MS for room in a
//   write, keeping what was not taken for next time, so it always sees a
//   quit or a change of voices;
// - REFUSED: the device would not open, a write failed, or the sink
//   STALLED (the stream says it plays and the ear has not moved for
//   OS64_MEDIA_OUTPUT_STALL_US past the start's hold bound, with sound
//   written; a stream someone else paused holds, and is no stall). It
//   is closed and tried again every OS64_MEDIA_OUTPUT_RETRY_MS. Meanwhile a
//   voice WAITs (`busy`, its clock held) or PLAYs ON (its clock monotonic)
//   as its owner chose.
// - at most OS64_MEDIA_OUTPUT_VOICES are mixed at once; one more that
//   wants sound is `busy` until one leaves.
//
// LOCKS: the output's own lock is taken before a voice's (the owner's), and
// never after one: an owner never calls into the output while holding its
// lock. Every field of a voice but the audio thread's private part is
// guarded by the owner's lock.

#include <stdbool.h>
#include <stdint.h>
#include "os64/lock.h"
#include "os64/media_clock.h"
#include "os64/media_mix.h"
#include "os64/media_queue.h"

#pragma GCC visibility push(default)

#define OS64_MEDIA_OUTPUT_VOICES           16
#define OS64_MEDIA_OUTPUT_WRITE_PATIENCE_MS 20
#define OS64_MEDIA_OUTPUT_RETRY_MS         500
#define OS64_MEDIA_OUTPUT_IDLE_CLOSE_MS    3000
#define OS64_MEDIA_OUTPUT_PAUSE_KEEP_MS    3000
#define OS64_MEDIA_OUTPUT_STALL_US         1000000

// What a voice does while the device is refused.
#define OS64_MEDIA_VOICE_WAIT     0   // `busy`; its clock holds (a sound alone: <audio>)
#define OS64_MEDIA_VOICE_PLAY_ON  1   // its clock goes monotonic, and back when the device returns (a picture)

typedef struct os64_media_output os64_media_output_t;

typedef struct os64_media_output_voice {
    // Fixed by the owner before os64_media_output_add, never changed after.
    os64_lock_t *lock;              // the owner's: every field below but `priv` is read and written under it
    os64_media_queue_t *queue;      // the voice's sound (frames on the media's timeline)
    os64_media_clock_t *clock;      // the owner's clock, or NULL: then `position` is the voice's time
    int refused;                    // OS64_MEDIA_VOICE_WAIT or _PLAY_ON
    void (*look)(void *ctx);        // a fact below changed; called under `lock`: rings a bell, nothing more
    void (*room)(void *ctx);        // frames were taken from `queue`; called under `lock`: rings the filler's bell
    void *ctx;

    // Asked by the owner, under `lock`.
    bool wants;                     // to be heard: not muted, not closed; with no clock, also playing
    bool runs;                      // with a clock: it plays (or plays out an end), so frames may be
                                    // written; a voice that wants and does not run holds the device
                                    // paused if nobody else plays. With no clock, `wants` is both
    bool remove;                    // the owner is done: the output lets go, then sets `gone`
    double gain;                    // 0 to 1; 0 is a silent voice, still mixed (only `wants` false removes it)
    int64_t sync;                   // frames added to every report to `clock` (gplay's sync dial)

    // Told by the output, under `lock`. The owner reads them; `look` says when they moved.
    bool gone;                      // the output will never touch this voice again: the owner may free it
    bool sounding;                  // the device is playing this voice's frames now
    bool busy;                      // it wants sound and cannot have it (a WAIT voice refused, or the mix full)
    bool refused_device;            // the device is refused (for every voice, whatever its setting)
    const char *refused_why;        // a string literal, or NULL
    bool heard_all;                 // the queue has finished and the ear has heard its last frame; an owner
                                    // that flushes its queue clears it (what it said is of the old queue)
    bool unwritten;                 // frames taken from the queue are not yet in the device (an end waits)
    int64_t position;               // with no clock: the media frame the ear is at, -1 until known
    bool rewind;                    // with no clock: a long pause gave the device back with frames of this
    int64_t rewind_to;              // voice unheard in it; the owner may go back to `rewind_to` and clear it
    uint32_t delay_us;              // the sink's reported delay, from the last position read
    bool delay_known;
    uint64_t written, dropped;      // frames written to the device; frames let go unheard
    uint64_t refusals, stalls;      // the device's, while this voice was in the output

    // The audio thread's alone; the owner never touches it.
    struct {
        struct os64_media_output_voice *next;
        bool mixed;                 // in the mix (keeps `gone` false)
        os64_media_voice_t anchors;
        uint32_t generation;        // the queue generation its anchors and reports describe
        uint32_t clock_generation;  // the clock's generation then
        uint64_t last_heard;        // the ear's stream frame at the last report
        struct {
            uint32_t offset, frames;
        } piece[8];                 // where its frames lie in the block last written
        uint32_t pieces;            // 0 once that block is all in the device
        bool stale;                 // a clock voice stopped wanting sound: its frames in the device are from before
    } priv;
} os64_media_output_voice_t;

// Starts the audio thread; the device is opened when a voice first wants
// sound. Returns 0, or OS64_MEDIA_NO_MEMORY with nothing started.
int os64_media_output_create(os64_media_output_t **out);

// Ends the audio thread and waits for it: at most a write's patience and a
// round. Every voice must have been removed and be `gone` first; with one
// still added it answers OS64_MEDIA_BAD_ARGS and changes nothing.
int os64_media_output_destroy(os64_media_output_t *o);

// Adds a voice the owner has filled in (its told fields zeroed, `position`
// -1). Not under the owner's lock. Returns 0, or OS64_MEDIA_BAD_ARGS.
int os64_media_output_add(os64_media_output_t *o, os64_media_output_voice_t *v);

// Rings the audio thread: an owner changed what it asks (play, pause, a
// seek's flush, remove). Not under the owner's lock.
void os64_media_output_ring(os64_media_output_t *o);

// The start's hold bound for a sink of `delay_us`: the first advancing
// report cannot come before the delay has passed, so the larger of
// OS64_MEDIA_CLOCK_START_HOLD_US and the delay plus 150 ms. The output's
// stall rule waits this long first; an owner gives it to its clock
// (os64_media_clock_hold).
int64_t os64_media_output_hold_bound(uint32_t delay_us);

typedef struct {
    bool open;                      // the device is held
    bool refused;
    const char *refused_why;
    uint32_t voices, mixed;         // added; in the mix now
    uint32_t delay_us;
    uint64_t written, refusals, stalls;
} os64_media_output_facts_t;

void os64_media_output_facts(os64_media_output_t *o, os64_media_output_facts_t *out);

#pragma GCC visibility pop

#endif // OS64_MEDIA_OUTPUT_H
