// media_pictures.c — pictures waiting their turn (os64/media_pictures.h).
//
// A ring of the caller's slots: the front is `head`, `count` follow it.

#include "os64/media_pictures.h"

void os64_media_pictures_init(os64_media_pictures_t *q, os64_media_pictures_entry_t *slot, uint32_t capacity)
{
    q->slot = slot;
    q->capacity = slot != 0 ? capacity : 0;
    q->head = 0;
    q->count = 0;
    q->generation = 0;
    q->ended = false;
}

uint32_t os64_media_pictures_count(const os64_media_pictures_t *q)
{
    return q->count;
}

bool os64_media_pictures_full(const os64_media_pictures_t *q)
{
    return q->count == q->capacity;
}

uint32_t os64_media_pictures_generation(const os64_media_pictures_t *q)
{
    return q->generation;
}

int os64_media_pictures_push(os64_media_pictures_t *q, uint32_t generation, void *handle, int64_t at,
                             int64_t duration)
{
    if (generation != q->generation || q->ended || handle == 0)
        return OS64_MEDIA_PICTURES_REFUSED;
    if (q->count == q->capacity)
        return OS64_MEDIA_PICTURES_FULL;
    q->slot[(q->head + q->count) % q->capacity] = (os64_media_pictures_entry_t){handle, at, duration};
    q->count++;
    return OS64_MEDIA_PICTURES_TAKEN;
}

const os64_media_pictures_entry_t *os64_media_pictures_peek(const os64_media_pictures_t *q, uint32_t index)
{
    if (index >= q->count)
        return 0;
    return &q->slot[(q->head + index) % q->capacity];
}

int64_t os64_media_pictures_newest(const os64_media_pictures_t *q)
{
    if (q->count == 0)
        return -1;
    return q->slot[(q->head + q->count - 1) % q->capacity].at;
}

bool os64_media_pictures_take(os64_media_pictures_t *q, os64_media_pictures_entry_t *out)
{
    if (q->count == 0)
        return false;
    if (out != 0)
        *out = q->slot[q->head];
    q->head = (q->head + 1) % q->capacity;
    q->count--;
    return true;
}

uint32_t os64_media_pictures_flush(os64_media_pictures_t *q, void (*release)(void *context, void *handle),
                                   void *context)
{
    os64_media_pictures_entry_t e;
    while (os64_media_pictures_take(q, &e))
        if (release != 0)
            release(context, e.handle);
    q->head = 0;
    q->ended = false;
    return ++q->generation;
}

bool os64_media_pictures_end(os64_media_pictures_t *q, uint32_t generation)
{
    if (generation != q->generation)
        return false;
    q->ended = true;
    return true;
}

bool os64_media_pictures_ended(const os64_media_pictures_t *q)
{
    return q->ended;
}

bool os64_media_pictures_finished(const os64_media_pictures_t *q)
{
    return q->ended && q->count == 0;
}
