// media_queue.c — decoded sound waiting to be played (os64/media_queue.h).
//
// A ring of stereo frames and, beside it, the segments that say which
// output frame each run of them is. The ring's front is `head`; the
// segments' frames add up to `count`, front segment first.

#include "os64/media_queue.h"

void os64_media_queue_init(os64_media_queue_t *q, int16_t *storage, uint32_t capacity)
{
    q->storage = storage;
    q->capacity = storage != 0 ? capacity : 0;
    q->head = 0;
    q->count = 0;
    q->generation = 0;
    q->ended = false;
    q->segments = 0;
}

uint32_t os64_media_queue_count(const os64_media_queue_t *q)
{
    return q->count;
}

uint32_t os64_media_queue_room(const os64_media_queue_t *q)
{
    return q->capacity - q->count;
}

uint32_t os64_media_queue_push(os64_media_queue_t *q, uint32_t generation, const int16_t *pcm, uint32_t frames,
                               int64_t at)
{
    if (generation != q->generation || q->ended || frames == 0 || at < 0)
        return 0;
    uint32_t room = q->capacity - q->count;
    if (frames > room)
        frames = room;
    if (frames == 0)
        return 0;
    os64_media_queue_segment_t *last = q->segments > 0 ? &q->segment[q->segments - 1] : 0;
    if (last != 0 && last->at + (int64_t)last->frames == at) {
        last->frames += frames;
    } else {
        if (q->segments == OS64_MEDIA_QUEUE_SEGMENTS)
            return 0;
        q->segment[q->segments++] = (os64_media_queue_segment_t){frames, at};
    }
    uint32_t tail = (q->head + q->count) % q->capacity;
    for (uint32_t i = 0; i < frames; i++) {
        q->storage[2 * tail] = pcm[2 * i];
        q->storage[2 * tail + 1] = pcm[2 * i + 1];
        if (++tail == q->capacity)
            tail = 0;
    }
    q->count += frames;
    return frames;
}

uint32_t os64_media_queue_take(os64_media_queue_t *q, int16_t *out, uint32_t frames, int64_t *at)
{
    if (q->segments == 0 || frames == 0)
        return 0;
    os64_media_queue_segment_t *front = &q->segment[0];
    if (frames > front->frames)
        frames = front->frames;
    if (at != 0)
        *at = front->at;
    if (out != 0)
        for (uint32_t i = 0; i < frames; i++) {
            uint32_t from = (q->head + i) % q->capacity;
            out[2 * i] = q->storage[2 * from];
            out[2 * i + 1] = q->storage[2 * from + 1];
        }
    q->head = (q->head + frames) % q->capacity;
    q->count -= frames;
    front->frames -= frames;
    front->at += frames;
    if (front->frames == 0) {
        for (uint32_t i = 1; i < q->segments; i++)
            q->segment[i - 1] = q->segment[i];
        q->segments--;
    }
    return frames;
}

int64_t os64_media_queue_front(const os64_media_queue_t *q)
{
    return q->segments > 0 ? q->segment[0].at : -1;
}

uint32_t os64_media_queue_flush(os64_media_queue_t *q)
{
    q->head = 0;
    q->count = 0;
    q->segments = 0;
    q->ended = false;
    return ++q->generation;
}

void os64_media_queue_end(os64_media_queue_t *q)
{
    q->ended = true;
}

bool os64_media_queue_ended(const os64_media_queue_t *q)
{
    return q->ended;
}

bool os64_media_queue_finished(const os64_media_queue_t *q)
{
    return q->ended && q->count == 0;
}
