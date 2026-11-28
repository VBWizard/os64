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
// call here is that thread's but `shown` and `shown_done`, which any thread
// may make while the player is open (and must have stopped making before
// os64_media_player_close). At
// each display frame it asks for the picture to show at that frame's blank
// (unless the player has a plane, whose show thread asks: § A plane,
// below), and it reads the facts when the player asks to be looked at. The library knows nothing of windows, keys, pages
// or the DOM.
//
// THE RULES are the cores' (os64/media_pictures.h, media_clock.h,
// media_pace.h) and VIDEO.md § 3's (as built in V4's gplay, in the library
// since V.5.2):
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
// "the facts changed, come and read": it is rung for a new state and for
// readiness moving (current, future, enough), so a caller that reads the
// facts only when told misses neither.
//
// A SEEK NOBODY ASKED FOR: when the output lets go of the player's sound
// still in the device (os64/media_output.h's `rewind`) while it is paused or
// buffering, the decoding thread seeks to where the clock stands, so the
// sound is queued again; that seek moves the generation facts.seeked takes,
// as any seek does.
//
// A PICTURE NOBODY ASKS FOR: while playing with no `frame` call for
// OS64_MEDIA_PLAYER_TICK_US (a covered window, a box out of view, a caller
// held by a page's layout), the picture thread retires what the clock has
// passed, leaving the shown picture alone, so the queues keep moving and
// the sound plays on. Playing or paused, a picture owed after a seek (or
// the open) lands unshown then, so facts.seeked moves with nobody watching.
//
// LOCKS: the player's lock is taken after its output's, never before; the
// player never calls into the output while holding it, and makes no GUI
// call while holding it.
//
// A PLANE (docs/design/pending/VIDEO_PLANE.md § 4; os64/gui.h § Planes):
// os64_media_player_plane gives the player a plane of a window, and a SHOW
// THREAD of its own makes it, publishes the picture due to it at the
// display's pace, and from then on is the pace's ONE owner: the caller's
// os64_media_player_frame is refused (false, counted in the facts) while
// the plane is on. The caller places and hides the plane in its own window
// publishes, says when it means to place it (os64_media_player_plane_want),
// and paints the video itself, when it must, from os64_media_player_shown.
// - THE FIRST PLACE waits for facts.plane_ready: the kernel refuses a place
//   for a plane with no picture of the place's size.
// - A NEW SIZE (§ 4.4): after os64_media_player_size a placed plane goes
//   STALE (a placed plane keeps its size) once a picture of the new size is
//   picked; the caller paints `shown` itself and hides the plane; the show
//   thread gives the plane the new size (a new plane past the old one's
//   class) and says READY; the caller wants it and places it again. A
//   hidden plane is simply given the new size. A new plane refused hands
//   the pace back to `frame`.
// - A PLACED PLANE SHOWS THROUGH whatever the window paints inside its clip,
//   outside its overlays, so a caller that stops wanting the picture there
//   (the element gone, a new page) hides the plane in the publish that
//   paints the place without it. A plain publish never hides a plane.
// - CLOSING: the show thread waits for the caller's hide, then destroys the
//   plane and ends; still placed after OS64_MEDIA_SHOW_CLOSE_BOUND_US
//   (os64/media_show.h) it ends with the plane placed, and the kernel takes
//   the plane out as its thread ends: the hole then shows the window's
//   content as it stands until the window repaints (counted in the facts).
//   `finished` comes after the show thread's end either way.

#include <stdbool.h>
#include <stdint.h>
#include "os64/bell.h"
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
    // NULL, or the source's first step, on the decoding thread before the
    // open (a file to open, a fetch to begin and its head to wait for, all
    // of which may block): it fills `source` in and answers true, or false
    // to fail the open (OS64_MEDIA_IO). With it `source` may start empty.
    bool (*prepare)(void *ctx, os64_media_source_t *source);
    uint32_t want;                  // OS64_MEDIA_WANT_SOUND and/or _PICTURE
    os64_media_output_t *output;    // NULL: the sound is not opened (the clock monotonic)
    int refused;                    // OS64_MEDIA_VOICE_WAIT or _PLAY_ON (os64/media_output.h); PLAY_ON is
                                    // for a media with a picture to show: one with sound alone WAITs,
                                    // its clock the ear, standing still until the device comes back
    bool loaded;                    // the whole source is at hand (a file); else os64_media_player_loaded says
    void (*look)(void *ctx);        // the facts changed (and, while a sound alone plays, a quarter second
                                    // went by: its position moved); called by the player's threads and its
                                    // output's, under the player's lock: rings the caller's bell, nothing more
    void (*finished)(void *ctx);    // after close: the source will never be read again and the player is
                                    // freed; called from a player thread, rings a bell, may not touch it
    void *ctx;
    os64_bell_t *bell;              // NULL, or the caller's bell for the decoding thread to wait on (its
                                    // source's read may wait on it too: one waiter, the same thread). The
                                    // player rings it as its own and never closes it; it must stay open
                                    // until `finished`. One handle fewer per player, where a page holds
                                    // dozens
} os64_media_player_config_t;

// Starts the decoding thread, which prepares the source if asked and opens
// it (either may read the network, so neither is done on the caller's
// thread). The player is
// OPENING, then PAUSED at frame 0 (BUFFERING if play was asked meanwhile),
// or FAILED with facts.failure saying why; failed or not, it is the
// caller's until os64_media_player_close. Returns 0, or
// OS64_MEDIA_BAD_ARGS / OS64_MEDIA_NO_MEMORY with nothing started.
int os64_media_player_create(const os64_media_player_config_t *cfg, os64_media_player_t **out);

// Returns at once; the pointer is dead on return. The player stops its
// threads, has its voice removed and releases every picture; when all of
// that is done it calls `finished` and frees itself. A decoding thread
// blocked in the source's read ends when the read returns, so the CALLER
// makes its source's read fail once it has closed (a file's read returns by
// itself).
void os64_media_player_close(os64_media_player_t *p);

// From PAUSED: through buffering, which ends at once when its exit holds;
// from ENDED: from the start. Asked while OPENING, it is carried out once
// the media is open.
void os64_media_player_play(os64_media_player_t *p);
void os64_media_player_pause(os64_media_player_t *p);
// To `frame` (output frames), held to the media, playing on if it was.
// Returns the seek's generation, which facts.seeked takes once the picture
// owed is shown (or, with no picture, sound from there is queued). Asked
// while OPENING, it is carried out once the media is open, under the
// generation returned; the last one asked wins.
uint32_t os64_media_player_seek(os64_media_player_t *p, int64_t frame);
// By `delta` frames from where a seek under way is going, or else from the
// clock.
uint32_t os64_media_player_seek_by(os64_media_player_t *p, int64_t delta);
// With a picture the voice goes and the clock is monotonic; a sound alone
// is muted by a gain of 0, a silent voice still mixed, its clock the ear
// (with no ear its queue would never drain).
void os64_media_player_mute(os64_media_player_t *p, bool muted);
void os64_media_player_gain(os64_media_player_t *p, double gain);   // 0 to 1; 0 a silent voice, still mixed
void os64_media_player_sync(os64_media_player_t *p, int64_t frames); // added to every heard report
// At the end, from the start again and playing on, without a gap: the
// decoding thread seeks the media to its start as it reaches the end and
// goes on queueing, on one timeline that does not turn back (each run's
// frames come after the last's), so the sound and the pictures run on and
// the clock never steps; facts.position is folded into the media's length.
// A run that gave nothing ends the media, as with loop off. Turned on once
// the end can no longer be taken back (a picture queue told it, or the
// output has taken the sound's last frame from its queue), the media plays
// on from its start after a
// gap, still with no end. Turned off, the last run queued ends the media:
// runs already queued after the one being heard (as many as the sound
// queue's two seconds hold) play first.
void os64_media_player_loop(os64_media_player_t *p, bool loop);
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
// with `out` empty when there is no picture yet, and while a plane is on
// (its show thread owns the pace; counted in facts.frame_refused).
bool os64_media_player_frame(os64_media_player_t *p, int64_t blank, int64_t period, os64_media_player_frame_t *out);

// What converting pictures has cost (the scaler's call alone): how many,
// their microseconds in total and the worst, and the last one's sizes and
// way (os64/media.h's OS64_MEDIA_SCALED_*), so a reader can tell a shrink
// from an enlargement and from a picture at its own size.
typedef struct {
    uint64_t count, total_us, worst_us;
    uint32_t w, h;                  // converted to
    uint32_t src_w, src_h;          // converted from
    int way;                        // OS64_MEDIA_SCALED_*
} os64_media_player_conversion_t;

typedef struct {
    int state;                      // OS64_MEDIA_PLAYER_*
    bool probed;                    // `info` holds (metadata)
    os64_media_info_t info;
    bool has_sound;                 // a sound this player decodes (playable and wanted)
    bool muted;
    int64_t length;                 // output frames, -1 when not known; the end sets it if it was not
                                    // known, and for a sound alone played from its start corrects it
                                    // when more than a tenth of a second away (with a picture, or after
                                    // a seek, the end is no evidence: see the decoding thread)
    int64_t length_us, looked_us;   // micros() when `length` was last set, and `look` last rung
    int64_t position;               // the clock's reading now; the length once ENDED, when known
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
    // The converted pictures' storage: how many hold pixels and how many
    // bytes, which grows with the size asked and comes back within a few
    // pictures of one under two thirds as large (frame_fit's rule).
    uint32_t frames_held;
    uint64_t frames_bytes;
    os64_media_player_conversion_t convert;
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
    // The plane (OS64_MEDIA_PLANE_*), as the show thread holds it.
    int plane_state;
    uint32_t plane_name;            // the GUI's name for it, 0 none
    int plane_refusal;              // REFUSED: the GUI's answer when none could be had (0: no GUI answer,
                                    // the show thread could not start or the start tripwire passed);
                                    // ENDED: the frame wait's answer that ended it (0: the player's close)
    uint32_t plane_cap_w, plane_cap_h, plane_pic_w, plane_pic_h;
    bool plane_ready;               // it holds a picture of the size being converted to (the size asked)
    bool plane_stale;               // a picture of another size was picked while it was placed; until the
                                    // publish that gives it that size
    uint32_t plane_serial;          // the serial of the picture the plane holds (`shown`'s numbering), 0
                                    // none: a caller places only when it is `shown`'s, or the glass would
                                    // step back to an older picture
    uint64_t plane_published, plane_recreates;
    uint64_t plane_ended_placed;    // closings that ended with the plane still placed
    uint64_t frames_missed;         // display frames passed with no answer, playing and shown
    uint64_t plane_placed_published;    // pictures published while the plane was placed and shown (its wait
                                        // answered a display frame) and the clock ran: what reached the glass
                                        // from it (`plane_published` counts hidden ones too)
    uint64_t plane_placed_runs;         // the runs of them begun (a run ends when the plane is not placed,
                                        // not shown, or the clock stops)
    int64_t plane_placed_first_us;      // micros() of the first of the latest run; 0 none yet
    int64_t plane_placed_prev_last_us;  // micros() of the last of the run before it; 0 none
    int64_t plane_placed_last_us;       // micros() of the last of them, kept after the run ends; 0 none yet
    uint64_t waits_patience;        // the show thread's waits ended by their patience
    uint64_t frame_refused;         // the caller's frame calls while the plane paced
    uint64_t plane_start_tripwires;
} os64_media_player_facts_t;

void os64_media_player_facts(os64_media_player_t *p, os64_media_player_facts_t *out);

// The plane's states (facts.plane_state).
#define OS64_MEDIA_PLANE_NONE      0   // no plane asked for
#define OS64_MEDIA_PLANE_STARTING  1   // the plane call is waiting for its verdict
#define OS64_MEDIA_PLANE_ON        2   // made: the show thread paces
#define OS64_MEDIA_PLANE_REFUSED   3   // none could be had (facts.plane_refusal): `frame` is the caller's
#define OS64_MEDIA_PLANE_ENDED     4   // the show thread has ended (closing, or the window gone): `frame` is the caller's

// Deliver to a plane of `win` (a window of this task). Waits for the show
// thread's verdict: 0, the plane made; OS64_MEDIA_NO_PLANE, none (the
// GUI's reason in facts.plane_refusal), the player going on as without one;
// OS64_MEDIA_BAD_ARGS without a picture asked for; OS64_MEDIA_BUSY asked
// while a plane is starting, on or refused, or while closing. A plane that
// ENDED while the player plays may be asked for again: its show thread is
// joined and a new one starts. The wait is as long as a create: milliseconds for the small
// classes, more to zero a large pair, so a caller whose thread must not
// wait asks from one that may.
int os64_media_player_plane(os64_media_player_t *p, int64_t win);
// The caller means to place the plane (or no longer does): the show thread
// keeps the plane's picture current while it is hidden.
void os64_media_player_plane_want(os64_media_player_t *p, bool want);
// The longest time, in microseconds, between two pictures in one run of
// those counted by facts.plane_placed_published, among the gaps ended since
// the last call (0: none). A gap open at a call is counted whole by the
// call after the picture that ends it. A caller measuring how long the
// glass went without a new picture asks it once an interval.
int64_t os64_media_player_plane_gap_take(os64_media_player_t *p);

typedef struct {
    const uint32_t *pixels;         // XRGB rows, w * h, unchanged until os64_media_player_shown_done
    uint32_t w, h;
    uint32_t serial;                // the frame call's serial for the same picture
    void *frame;                    // the player's
} os64_media_player_view_t;

// The picture the pace last picked, PINNED for the caller (any thread) until
// os64_media_player_shown_done: its pixels do not change and it is not
// reused, while the pace moves on. Moves nothing in the pace. At most two
// views are held at once (OS64_MEDIA_BUSY past them); OS64_MEDIA_BAD_ARGS
// with no picture yet. Give every view back before close: a view is dead
// after it.
int os64_media_player_shown(os64_media_player_t *p, os64_media_player_view_t *out);
void os64_media_player_shown_done(os64_media_player_t *p, os64_media_player_view_t *view);

#pragma GCC visibility pop

#endif // OS64_MEDIA_PLAYER_H
