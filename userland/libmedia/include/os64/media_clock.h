#ifndef OS64_MEDIA_CLOCK_H
#define OS64_MEDIA_CLOCK_H

// os64/media_clock.h — what time it is in a playing media (libmedia.so).
//
// A video player asks it one question, many times a second: what media
// time is showing at wall time `t`? The main loop asks for the blank its
// next publish will be shown at; the picture thread asks for the blank a
// conversion could reach. Times in the media are OUTPUT frames (48 kHz,
// os64/media.h's timeline); wall times are micros() (os64/ticks.h).
//
// TWO SOURCES. HEARD follows what the ear hears: the player reports "frame
// H reached the ear at wall Th", from /dev/audiopos's position with the
// sink's delay already taken off, so this clock does no delay arithmetic.
// MONOTONIC runs at real time from where it was anchored, for a media with
// no sound, sound muted, sound refused or sound ended before the picture.
// The player chooses the source; the clock never chooses for it.
//
// A READING NEVER MOVES BACKWARD AND NEVER STEPS, except where it is moved
// on purpose: a seek is told where to go, and a report that finds it behind
// the ear by more than THE BOUND (below) steps it forward. Otherwise it
// rises with `t`, and an event leaves the reading at its own `now` where it
// was. A reading taken AHEAD, for a `t` past the next event, is a forecast
// that event may revise down (a starve freezes the reading at the starve's
// moment; a report may turn the slew from fast to slow, or hold it); a picture taken on the forecast is simply gone, which is
// why the forecast is never more than a blank or a conversion ahead.
// Each source has its own view of the time, and where
// they disagree (a change of source, a report that says the ear is
// elsewhere) the difference, the DRIFT, is worked off by running a
// twentieth fast or slow (the SLEW: 50 ms of disagreement takes a second),
// down to the last 10 ms, which go at a 480th (4.8 s).
// A twentieth is under the pitch change anyone hears and under a picture's
// worth of drift a second at 60 pictures a second. The last 10 ms are the
// size of a sink's report scatter, which at a twentieth swung the reading
// enough from report to report to leave pictures unshown.
//
// HOLDS. The reading stands still in four places, and each has an end:
// - STARTING: after play or seek with the heard source, the reading holds
//   until the first ADVANCING report of the current generation, so the
//   picture does not run ahead of a sound the device has not yet made
//   heard. If none has come within the hold's BOUND it runs at real time
//   from where it held, so a silent device never freezes the picture; the
//   first advancing report then becomes drift. The bound is the player's
//   (os64_media_clock_hold), because the first report cannot come before
//   the sink's delay has passed: the larger of
//   OS64_MEDIA_CLOCK_START_HOLD_US and that delay plus 150 ms. A change to
//   the heard source while running (unmute) does NOT hold: a picture
//   already moving keeps moving, at real time, until the first report.
// - STARVED: a report that says the ear is not advancing (the sound ran
//   dry, the stream was held) holds the reading, drift and all, because
//   the sound is the master. IT HOLDS FOR AS LONG AS IT IS TOLD: a fresh
//   advancing report or os64_media_clock_source(MONOTONIC) ends it, and
//   the PLAYER owns the rule that a starved hold does not last for ever.
// - A STILL EAR: following, a report is trusted for the player's still_us
//   past the moment it describes; with no newer one by then the ear is
//   taken as stopped, the reading holds where that left it, and the state
//   reads STILL, until a report says the ear moved. A sound gap of any
//   length is then a picture held for it, less still_us: nothing was lost,
//   the sound is later by the gap, and so is the picture.
// - AHEAD BY MORE THAN THE BOUND: a report finding the reading more than
//   OS64_MEDIA_CLOCK_BOUND_US ahead of the ear holds the reading until the
//   ear is within it; the slew takes the rest. One finding it more than
//   the bound BEHIND steps it forward to the bound, the one step forward a
//   report may make (the pace drops what it passes). The bound applies at
//   every advancing report, whatever the clock was doing before it. So
//   following, the drift is never more than the bound once a hold has
//   ended, whatever the sound does; during a hold the reading is ahead by
//   what the report found (a gap can carry it the trust past where the ear
//   stopped: the bound and the trust at most), shrinking at real time. The
//   slew bounds how fast drift is corrected; this bounds how much there
//   can be.
//
// GENERATIONS. A seek starts a new generation and forgets every report. A
// report names the generation of the sound it measured, and one from
// before the seek is refused.
//
// PURE, AND NOT LOCKED: it calls nothing and keeps no time of its own, so
// tools/test_media_cores_host.sh drives it with made-up clocks. A reading
// is a function of what it has been told and of `t` alone: two threads
// reading for different `t` get each their own answer. A `t` (or a `now`)
// earlier than the last event it was told is taken as that event's
// moment. A caller whose threads share one holds a lock of its own across
// every call.
//
// THE PLAYBACK RATE is one, and lives in one place (media_clock.c's
// `run`), where a rate would be one factor (DEBTS: playback rate).

#include <stdbool.h>
#include <stdint.h>

#pragma GCC visibility push(default)

#define OS64_MEDIA_CLOCK_MONOTONIC  0
#define OS64_MEDIA_CLOCK_HEARD      1

// The least a start on the heard source holds for its first advancing
// report before running anyway: a quarter second, the bound a new clock
// has, to tune on the P5.
#define OS64_MEDIA_CLOCK_START_HOLD_US 250000

// How long past the moment a report describes the ear is trusted to be
// moving, until the player says otherwise (os64_media_clock_still). It is
// not a taste: it is longer than the oldest a healthy report can be when
// the reading is taken, which is the player's arithmetic (its sink's pull,
// the pass that pull waits for, its own round, how far ahead it reads).
// 80 ms is gplay's on the headphones.
#define OS64_MEDIA_CLOCK_STILL_US 80000

// THE BOUND: following, the reading is never more than this from the ear's
// view at a report (the view held where the ear stopped): about where lip
// sync becomes visible.
#define OS64_MEDIA_CLOCK_BOUND_US 100000

// Report ages, for a readout: up to 20, 40, 60, 80 and 120 ms, and over.
#define OS64_MEDIA_CLOCK_AGE_BUCKETS 6

// What the clock is doing at a wall time (os64_media_clock_state).
#define OS64_MEDIA_CLOCK_PAUSED         0
#define OS64_MEDIA_CLOCK_RUNNING        1   // monotonic
#define OS64_MEDIA_CLOCK_STARTING       2   // heard: holding for the first advancing report
#define OS64_MEDIA_CLOCK_STARTING_LATE  3   // heard: the hold ran out; running at real time
#define OS64_MEDIA_CLOCK_AWAITING       4   // heard, changed to while running: real time until a report
#define OS64_MEDIA_CLOCK_FOLLOWING      5   // heard: following the reports
#define OS64_MEDIA_CLOCK_STARVED        6   // heard: the last report was not advancing; holding
#define OS64_MEDIA_CLOCK_STILL          7   // heard: the last report outlived its trust; holding

typedef struct {
    int mode;               // media_clock.c's MODE_*
    int source;             // OS64_MEDIA_CLOCK_MONOTONIC or _HEARD: the player's choice
    uint32_t generation;
    int64_t since;          // wall: the last event
    int64_t base;           // the reading at `since`, drift included (units of media_clock.c);
                            // following, what it holds at while ahead by more than the bound
    int64_t hold_until;     // STARTING: wall when the hold ends
    int64_t hold_us;        // the bound a start's hold is given (os64_media_clock_hold)
    int64_t still_us;       // the last report's trust
    int64_t still_next;     // the trust the next report takes (os64_media_clock_still)
    int64_t heard;          // FOLLOWING: the reported frame (units) ...
    int64_t heard_wall;     // ... and when it reached the ear
    int64_t drift;          // FOLLOWING: the reading minus the heard view, at `slew_since` (units),
                            // never more than the bound either way
    int64_t slew_since;     // FOLLOWING: wall when the drift starts being worked off
    // For a readout; never reset.
    uint64_t reports;       // taken
    uint64_t reports_stale; // refused: from an earlier generation
    uint64_t starts_late;   // starts whose hold ran out before an advancing report
    uint64_t starves;       // times a report held the reading
    uint64_t stills;        // reports that came after the ear had been taken as stopped
    uint64_t held_ahead;    // reports after which the reading held, ahead by more than the bound
    uint64_t jumped_behind; // reports after which it stepped forward, behind by more
    int64_t start_wait_us;  // the last start's wait for its first advancing report
    uint64_t report_ages[OS64_MEDIA_CLOCK_AGE_BUCKETS]; // following: the last report's age when replaced
} os64_media_clock_t;

// Paused at frame 0, generation 0, on `source`.
void os64_media_clock_init(os64_media_clock_t *c, int source);

// Running from where it was paused (a hold first, on the heard source). No
// change when already running.
void os64_media_clock_play(os64_media_clock_t *c, int64_t now);
// Frozen at the reading for `now`. No change when already paused.
void os64_media_clock_pause(os64_media_clock_t *c, int64_t now);
// The reading is `frame` exactly, paused or running (a hold first, on the
// heard source); reports are forgotten. Returns the new generation.
uint32_t os64_media_clock_seek(os64_media_clock_t *c, int64_t frame, int64_t now);
// Changes the source without moving the reading. Paused, it takes effect
// at play.
void os64_media_clock_source(os64_media_clock_t *c, int source, int64_t now);
// The bound the next start's hold is given, in microseconds (a value
// below 1 is taken as 1). A start already holding keeps its own.
void os64_media_clock_hold(os64_media_clock_t *c, int64_t us);
// How long past the moment it describes a report is trusted, in
// microseconds (a value below 1 is taken as 1): OS64_MEDIA_CLOCK_STILL_US.
// It applies from the next advancing report, so a reading already held is
// never moved.
void os64_media_clock_still(os64_media_clock_t *c, int64_t us);

// Frame `frame` of `generation` reached the ear at wall `frame_wall`,
// reported at `now`; `advancing` false when the ear's position has stopped.
// True when the report was taken; false for one from another generation,
// one while paused or on the monotonic source, and, while starting or
// awaiting, one not advancing or heard before the wait began.
bool os64_media_clock_heard(os64_media_clock_t *c, uint32_t generation, int64_t frame, int64_t frame_wall,
                            bool advancing, int64_t now);

// The media frame showing at wall time `t`.
int64_t os64_media_clock_read(const os64_media_clock_t *c, int64_t t);
uint32_t os64_media_clock_generation(const os64_media_clock_t *c);
// OS64_MEDIA_CLOCK_PAUSED and the rest, at wall time `t`. Following, with
// the last report older than its trust, it is STILL: the ear has not been
// heard to stop (that is STARVED, from a report), it has only not been heard
// to move.
int os64_media_clock_state(const os64_media_clock_t *c, int64_t t);
// The drift still to be worked off at `t`, in frames rounded down: the reading minus
// what the reports alone say, both held where the ear stopped (positive:
// the reading is ahead of the ear); during a hold ahead, more than the
// bound. 0 when not following reports.
int64_t os64_media_clock_drift(const os64_media_clock_t *c, int64_t t);

#pragma GCC visibility pop

#endif // OS64_MEDIA_CLOCK_H
