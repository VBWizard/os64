#ifndef OS64_MEDIA_QUEUE_H
#define OS64_MEDIA_QUEUE_H

// os64/media_queue.h — decoded sound waiting to be played (libmedia.so).
//
// A player that decodes on one thread and writes to /dev/audio on another
// puts this between them: the decoder PUSHES what os64_media_decode gave
// it, the writer TAKES from the front. It holds a bounded number of frames
// (the stream's format, os64/audio.h), and it remembers WHERE IN THE MEDIA
// each frame came from, so the writer can say what is being heard: every
// frame carries its output frame number (os64/media.h's timeline).
//
//     decoder                               writer
//     n = os64_media_decode(m, pcm, k);     n = os64_media_queue_take(&q, out, k, &at);
//     lock; pushed = push(&q, gen, pcm,     unlock;
//                  n, at); unlock;          write n frames; the first is media frame `at`
//
// PURE, AND NOT LOCKED: it calls nothing and allocates nothing (the caller
// hands it its storage), so tools/test_media_queue_host.sh drives it
// against a model. A caller whose producer and consumer are different
// threads holds one lock of its own across every call.
//
// THE TIMELINE. Pushed frames are SEGMENTS: a push that continues the last
// one (its `at` is the frame after the last one queued) grows it; one that
// does not (a loop back to the start) begins another. A take never crosses
// from one segment into the next, so one take is one run of the media, and
// `at` says where it starts.
//
// GENERATIONS. A flush empties the queue and starts a new generation. A
// push names the generation it was decoded for, and one decoded before the
// flush is refused whole, so a decoder that was part way through a decode
// when the player seeked cannot land the old place's sound after the new.
//
// THE END. The decoder says when the media has no more (os64_media_queue_end);
// once what is queued has been taken, os64_media_queue_finished is true. A
// flush clears it, and so does a reopen (a loop turned on after the end was
// queued: there is more after all).

#include <stdbool.h>
#include <stdint.h>

#pragma GCC visibility push(default)

// How many segments one queue keeps apart. A push that would need one more
// takes nothing, as a full queue does, until a take ends a segment.
#define OS64_MEDIA_QUEUE_SEGMENTS 64

typedef struct {
    uint32_t frames;        // in this segment
    int64_t at;             // the output frame of its first
} os64_media_queue_segment_t;

typedef struct {
    int16_t *storage;       // the caller's: capacity * 2 samples
    uint32_t capacity;      // frames
    uint32_t head;          // where the front frame is in storage
    uint32_t count;         // frames held
    uint32_t generation;
    bool ended;
    uint32_t segments;      // in use, the front one first
    os64_media_queue_segment_t segment[OS64_MEDIA_QUEUE_SEGMENTS];
} os64_media_queue_t;

// An empty queue over `storage` (room for `capacity` stereo frames, which
// stays the caller's), generation 0.
void os64_media_queue_init(os64_media_queue_t *q, int16_t *storage, uint32_t capacity);

// Frames held, and frames of room.
uint32_t os64_media_queue_count(const os64_media_queue_t *q);
uint32_t os64_media_queue_room(const os64_media_queue_t *q);

// Up to `frames` frames from `pcm`, the first of them output frame `at`,
// decoded for `generation`. Returns how many were taken: all that fit, or
// 0 when the queue is full, when it is out of segments, when `generation`
// is not the queue's, or after os64_media_queue_end.
uint32_t os64_media_queue_push(os64_media_queue_t *q, uint32_t generation, const int16_t *pcm, uint32_t frames,
                               int64_t at);

// Up to `frames` frames off the front into `out` (NULL drops them),
// within one segment, with `*at` (if not NULL) the output frame of the
// first. Returns how many; 0 when empty.
uint32_t os64_media_queue_take(os64_media_queue_t *q, int16_t *out, uint32_t frames, int64_t *at);

// The output frame the front frame is, or -1 when empty.
int64_t os64_media_queue_front(const os64_media_queue_t *q);

// Empty it, forget the end, and begin the next generation, which it
// returns.
uint32_t os64_media_queue_flush(os64_media_queue_t *q);

// No more will be pushed this generation.
void os64_media_queue_end(os64_media_queue_t *q);
// The end taken back: pushes are taken again, this generation, after what
// is queued.
void os64_media_queue_reopen(os64_media_queue_t *q);
bool os64_media_queue_ended(const os64_media_queue_t *q);
// Ended, and everything pushed has been taken.
bool os64_media_queue_finished(const os64_media_queue_t *q);

#pragma GCC visibility pop

#endif // OS64_MEDIA_QUEUE_H
