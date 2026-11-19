#ifndef OS64_MEDIA_PACE_H
#define OS64_MEDIA_PACE_H

// os64/media_pace.h — which picture to show, and what to do when late
// (libmedia.so).
//
// A video player's threads ask it three things (VIDEO.md § 5 has the
// tables):
//
// AT EACH PAINT (the main loop, os64_media_pace_pick): given the clock's
// reading for the blank this paint will be shown at, SHOW the latest ready
// picture already due and DROP, unshown, every ready one before it: a late
// picture is never shown late. With nothing due, what is on the screen
// stays. With nothing ready and the picture on the screen past its end, the
// paint is STARVED, which is counted and changes nothing here.
//
// BEFORE EACH CONVERSION (the picture thread, os64_media_pace_skip): SKIP
// the front decoded picture when the one after it is already due at the
// blank this conversion could reach (os64_media_pace_reach). Its successor,
// not its duration, decides, because a file may state no duration; with no
// successor decoded it is converted.
//
// THE PRODUCERS' STATE (the main loop, os64_media_pace_judge, once a
// paint): from the decoder's LEAD, the newest decoded picture's time minus
// the clock, it moves between three states, and says what the decoding
// thread is to do:
//
//   on time        decode every picture
//   decoding late  leave out the pictures nothing depends on
//                  (os64_media_picture_skip UNUSED)
//   far behind     drop to the next keyframe (os64_media_picture_resync)
//                  and let the sound run; the screen HOLDS its last picture
//                  until that keyframe, which on the web is commonly two to
//                  five seconds away. That is the design: a picture shown
//                  late is worse. far_behind_us and the entries say how
//                  often it happens and what it cost.
//
// The doors are fractions of the decoded queue's DEPTH (its capacity), so
// they move with it: in to decoding late when the lead has been below
// three eighths of the depth for OS64_MEDIA_PACE_IN_HOLD_US, out when it
// has been three quarters or more for OS64_MEDIA_PACE_OUT_HOLD_US (the gap
// and the long hold are what keep it from flapping after a burst of cheap
// pictures). Far behind is at once: the lead more than
// OS64_MEDIA_PACE_FAR_PERIODS periods behind, or the sound queue under the
// player's floor, because the sound is what is protected. The sound's door
// fires once, then re-arms only when the sound has been back at its floor:
// a shortfall a resync did not cure is not the picture decoder's, and
// another resync would only cost the pictures again. It needs no period
// and no picture decoded, which the lead's doors do. Every hold is wall
// time, so a covered window, which paints nothing, holds nothing.
//
// A RESYNC IS ASKED, THEN TAKEN. Entering far behind sets `resync_asked`;
// the decoding thread, under the player's lock, calls
// os64_media_pace_resync_take, and resyncs when it answers true. A reset
// (a seek) withdraws one not yet taken, so a resync asked before a seek is
// never carried out after it.
//
// NO VERDICT WITHOUT SOMETHING TO JUDGE. The lead also falls to nothing at
// the end of the media (the newest decoded picture is the last), while the
// player is buffering after a seek, and while it is paused; the player
// says `judging` false then, and no door moves and no hold runs. A paint
// that is not judging is not counted starved either.
//
// PURE, AND NOT LOCKED: it calls nothing but os64/media_pictures.h's
// readers of the queues it is given, so tools/test_media_cores_host.sh
// drives it with made-up times.
// The player holds the lock its queues are under across every call.

#include <stdbool.h>
#include <stdint.h>

#include "os64/media_pictures.h"

#pragma GCC visibility push(default)

#define OS64_MEDIA_PACE_ON_TIME        0
#define OS64_MEDIA_PACE_DECODING_LATE  1
#define OS64_MEDIA_PACE_FAR_BEHIND     2

// The doors, to tune on the P5: in below IN_EIGHTHS / 8 of the depth, held
// IN_HOLD_US; out at OUT_EIGHTHS / 8 or more, held OUT_HOLD_US.
#define OS64_MEDIA_PACE_IN_EIGHTHS   3
#define OS64_MEDIA_PACE_IN_HOLD_US   125000
#define OS64_MEDIA_PACE_OUT_EIGHTHS  6
#define OS64_MEDIA_PACE_OUT_HOLD_US  1000000
// Far behind: this many periods behind, at once.
#define OS64_MEDIA_PACE_FAR_PERIODS  30

typedef struct {
    int state;              // OS64_MEDIA_PACE_*
    uint32_t depth;         // the decoded queue's capacity
    int64_t period;         // output frames a picture; 0 when not known (only the sound's door moves)
    int64_t in_since;       // wall when the lead went under the in door; -1 when not under
    int64_t out_since;      // the same for the out door
    int64_t far_since;      // wall when far behind began
    bool sound_armed;       // the sound's door may fire
    // What the decoding thread is to do, read under the player's lock.
    bool skip_unused;       // leave out the pictures nothing depends on
    bool resync_asked;      // os64_media_pace_resync_take takes it
    // For a readout; reset by nothing.
    int64_t lead;           // the last judged, in output frames
    uint64_t resyncs;       // asked
    uint64_t shown, dropped, skipped, starved;
    uint64_t entered[3];    // entries into each state, by OS64_MEDIA_PACE_*
    uint64_t far_by_sound;  // far-behind entries the sound's floor caused
    uint64_t far_behind_us; // wall time spent far behind, the screen holding
} os64_media_pace_t;

// On time, for a decoded queue `depth` deep and pictures `period` output
// frames apart (0 when the file states no rate: os64_media_pace_period
// gives it later).
void os64_media_pace_init(os64_media_pace_t *p, uint32_t depth, int64_t period);
void os64_media_pace_period(os64_media_pace_t *p, int64_t period);
// A seek or a new media: on time, nothing skipped, a resync not yet taken
// withdrawn, the sound's door armed; the counters stay.
void os64_media_pace_reset(os64_media_pace_t *p, int64_t now);
// Whether the decoding thread is to resync now; the ask is taken with it.
bool os64_media_pace_resync_take(os64_media_pace_t *p);

typedef struct {
    bool show;              // take `drop` from the front, unshown, then show the next one
    uint32_t drop;
    bool starved;
} os64_media_pace_pick_t;

// The paint's choice from `ready` at `clock`, the reading for the blank it
// will be shown at. `shown` is the picture on the screen, or NULL. The
// queue is not changed: the caller takes what the answer says.
os64_media_pace_pick_t os64_media_pace_pick(os64_media_pace_t *p, const os64_media_pictures_t *ready, int64_t clock,
                                            const os64_media_pictures_entry_t *shown, bool judging);

// The first blank at or after `now + cost` on the display's grid (a blank
// at `blank`, one every `period_us`), in wall time: the blank a conversion
// begun now could reach, `cost` being what the last conversion took. The
// caller reads the clock there for os64_media_pace_skip.
int64_t os64_media_pace_reach(int64_t now, int64_t cost, int64_t blank, int64_t period_us);
// Whether to skip the front of `decoded` (release it unconverted): its
// successor is due at `clock`.
bool os64_media_pace_skip(os64_media_pace_t *p, const os64_media_pictures_t *decoded, int64_t clock);

typedef struct {
    int64_t now;            // wall
    int64_t clock;          // the reading for this paint's blank
    bool judging;
    int64_t newest;         // the newest picture the decoder has given, its time; -1 for none
    uint32_t sound_frames;  // decoded sound queued
    uint32_t sound_floor;   // under this is far behind; 0 when the sound protects nothing
                            // (none, unheard, ended, or no picture still to come)
} os64_media_pace_measure_t;

// The producers' state after this paint's measure (OS64_MEDIA_PACE_*).
int os64_media_pace_judge(os64_media_pace_t *p, const os64_media_pace_measure_t *m);
// The first picture after a resync is decoded: far behind ends.
void os64_media_pace_resynced(os64_media_pace_t *p, int64_t now);

#pragma GCC visibility pop

#endif // OS64_MEDIA_PACE_H
