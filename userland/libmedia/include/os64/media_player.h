#ifndef OS64_MEDIA_PLAYER_H
#define OS64_MEDIA_PLAYER_H
// os64/media_player.h — one media playing: its threads, its queues, its
// clock, and the picture to show (libmedia.so).
//
// THE SOUND NEVER WAITS ON THE CALLER'S THREAD. Nothing that keeps sound
// flowing, starts it or recovers it needs the caller to answer a bell: the
// player's own threads, and its output's, do all of it. The caller is
// needed for the picture on the glass and for the facts, nothing else.
//
// A player opens a byte source for the kinds asked, decodes on a thread of
// its own, converts pictures to the size asked on another, and gives its
// sound to an output (os64/media_output.h) as a voice. The caller drives it
// from ONE thread of its own (gplay's main, yonder's window thread): every
// call here is that thread's. At each display frame it asks for the picture
// to show at that frame's blank, and it reads the facts when the player
// asks to be looked at. The library knows nothing of windows, keys, pages
// or the DOM.
//
// THE RULES are the cores' (os64/media_pictures.h, media_clock.h,
// media_pace.h) and VIDEO.md § 3, as built in gplay:
// - buffering until a picture is ready and half a second of sound is
//   queued (and the sink's delay known), then a start's hold;
// - the clock's trust and bound;
// - the pace's doors and resync;
// - a seek's generation, and a picture owed after it (the next frame shows
//   the first one ready, whatever its time);
// - collect-then-release (a picture goes back to libmedia outside the
//   player's lock);
// - a decoded picture kept by its frame, so a resize while paused or ended
//   converts the shown picture again.
//
// THE STATE MOVES WHERE IT CHANGES: buffering's end, a sound that stopped,
// and the end are decided under the player's lock by the thread that made
// them true (a push, a report, a conversion, the picture thread's timed
// wake), and again in each of the caller's calls. `look` then says only
// "the facts changed, come and read".
//
// A PICTURE NOBODY ASKS FOR: while playing with no `frame` call for
// OS64_MEDIA_PLAYER_TICK_US (a covered window, a box out of view, a caller
// held by a page's layout), the picture thread retires what the clock has
// passed, leaving the shown picture alone, so the queues keep moving and
// the sound plays on.
//
// LOCKS: the player's lock is taken after its output's, never before; the
// player never calls into the output while holding it.

#include <stdbool.h>
#include <stdint.h>
#include "os64/media.h"
#include "os64/media_clock.h"
#include "os64/media_output.h"
#include "os64/media_pace.h"

#pragma GCC visibility push(default)

#define OS64_MEDIA_PLAYER_TICK_US 100000

// The player's states (os64_media_player_facts_t's `state`).
#define OS64_MEDIA_PLAYER_OPENING    0   // the decoding thread is probing the source
#define OS64_MEDIA_PLAYER_BUFFERING  1   // play asked; filling, the clock paused
#define OS64_MEDIA_PLAYER_PLAYING    2
#define OS64_MEDIA_PLAYER_PAUSED     3
#define OS64_MEDIA_PLAYER_ENDED      4
#define OS64_MEDIA_PLAYER_FAILED     5

typedef struct os64_media_player os64_media_player_t;

typedef struct {
    os64_media_source_t source;     // read on the decoding thread only; see os64_media_player_close
    uint32_t want;                  // OS64_MEDIA_WANT_SOUND and/or _PICTURE
    os64_media_output_t *output;    // NULL: no voice (the sound decoded and let go, the clock monotonic)
    int refused;                    // OS64_MEDIA_VOICE_WAIT or _PLAY_ON (os64/media_output.h)
    bool loaded;                    // the whole source is at hand (a file); else os64_media_player_loaded says
    void (*look)(void *ctx);        // the facts changed; called by the player's threads and its output's,
                                    // under the player's lock: rings the caller's bell, nothing more
    void (*finished)(void *ctx);    // after close: the source will never be read again and the player is
                                    // freed; called from a player thread, rings a bell, may not touch it
    void *ctx;
} os64_media_player_config_t;

// Starts the decoding thread, which opens the source (a probe may read the
// network, so it is never done on the caller's thread). The player is
// OPENING, then PAUSED at frame 0 (BUFFERING if play was asked meanwhile).
// Returns 0, or OS64_MEDIA_BAD_ARGS / OS64_MEDIA_NO_MEMORY with nothing
// started.
int os64_media_player_create(const os64_media_player_config_t *cfg, os64_media_player_t **out);

// Returns at once; the pointer is dead on return. The player stops its
// threads, has its voice removed and releases every picture; when all of
// that is done it calls `finished` and frees itself. A decoding thread
// blocked in the source's read ends when the read returns, so the CALLER
// makes its source's read fail once it has closed (a file's read returns by
// itself).
void os64_media_player_close(os64_media_player_t *p);

// From PAUSED: playing (through buffering when no picture is ready or a seek
// is pending); from ENDED: from the start. Asked while OPENING, it is
// carried out once the media is open.
void os64_media_player_play(os64_media_player_t *p);
void os64_media_player_pause(os64_media_player_t *p);
// To `frame` (output frames), held to the media, playing on if it was.
// Returns the seek's generation, which facts.seeked takes once the picture
// owed is shown (or, with no picture, sound from there is queued).
uint32_t os64_media_player_seek(os64_media_player_t *p, int64_t frame);
// By `delta` frames from where a seek under way is going, or else from the
// clock.
uint32_t os64_media_player_seek_by(os64_media_player_t *p, int64_t delta);
void os64_media_player_mute(os64_media_player_t *p, bool muted);   // the voice goes; the clock monotonic
void os64_media_player_gain(os64_media_player_t *p, double gain);   // 0 to 1; 0 a silent voice, still mixed
void os64_media_player_sync(os64_media_player_t *p, int64_t frames); // added to every heard report
// The source is all at hand from now on (HTML's HAVE_ENOUGH_DATA needs it).
void os64_media_player_loaded(os64_media_player_t *p, bool loaded);

// The size pictures are converted to (the fitted rectangle, in pixels; the
// caller does any fitting). A new size while paused or ended converts the
// shown picture again, and the next frame call answers it as changed.
void os64_media_player_size(os64_media_player_t *p, uint32_t w, uint32_t h);

typedef struct {
    const uint32_t *pixels;         // XRGB rows, w * h; valid until the next frame call that answers
                                    // `changed`, or close: no other call moves them
    uint32_t w, h;                  // as converted (a frame from before a resize is its old size)
    uint32_t serial;                // moves whenever the picture changes
    bool changed;                   // `serial` moved since the last call
} os64_media_player_frame_t;

// The picture to show at `blank` (wall microseconds: the display frame's
// blank plus one period, when a publish now is seen), `period` the
// display's period in microseconds: the pace's pick, then its judge. False
// with `out` empty when there is no picture yet.
bool os64_media_player_frame(os64_media_player_t *p, int64_t blank, int64_t period, os64_media_player_frame_t *out);

typedef struct {
    int state;                      // OS64_MEDIA_PLAYER_*
    bool probed;                    // `info` holds (metadata)
    os64_media_info_t info;
    bool has_sound;                 // a sound this player decodes (playable and wanted)
    bool muted;
    int64_t length;                 // output frames, -1 when not known
    int64_t position;               // the clock's reading now
    // Readiness at the position, each implying the one before.
    bool current;                   // a picture for now is converted (with no picture: sound queued)
    bool future;                    // buffering's exit holds
    bool enough;                    // future, and the source loaded or every queue full
    uint32_t seeked;                // the last seek generation landed
    bool busy;                      // a WAIT voice, and the device refused (or the mix full)
    bool sounding;                  // the device plays this voice now
    bool refused;                   // the device is refused
    const char *refused_why;
    int failure;                    // a libmedia result, 0 none
    // Where the clock and the pace stand, for a readout.
    int clock_state;                // OS64_MEDIA_CLOCK_*
    int64_t drift;                  // frames
    int64_t shown_at, shown_end;    // the shown picture's time and end (-1 when none)
    uint32_t decoded, ready;        // pictures in each queue
    uint32_t decoded_depth, ready_depth;
    uint32_t sound_queued;          // frames
    uint32_t delay_us;              // the sink's
    int64_t sync;
    int64_t conversion_us;
    // The cores' own counters, copied, and the player's.
    os64_media_pace_t pace;
    os64_media_clock_t clock;
    os64_media_counts_t media;
    uint64_t pictures_decoded, conversions, reconversions, buffers;
    uint64_t sound_decoded, sound_written, sound_dropped, refusals, stalls;
} os64_media_player_facts_t;

void os64_media_player_facts(os64_media_player_t *p, os64_media_player_facts_t *out);

#pragma GCC visibility pop

#endif // OS64_MEDIA_PLAYER_H
