#ifndef OS64_MEDIA_PICTURES_H
#define OS64_MEDIA_PICTURES_H

// os64/media_pictures.h — pictures waiting their turn (libmedia.so).
//
// A video player keeps two of these: the DECODED pictures, which the
// decoding thread pushes as os64_media_next hands them out and the picture
// thread takes to convert, and the READY ones, converted to the window's
// pixels, which the picture thread pushes and the main loop shows. Each
// entry is a HANDLE the caller owns (an os64_media_picture_t, a buffer of
// pixels) with the media time it is shown at and for how long, in output
// frames (os64/media.h's timeline).
//
//     producer                                consumer
//     lock; g = generation(&q); unlock;       lock; take(&q, &e); unlock;
//     make the picture                        use e.handle
//     lock; r = push(&q, g, h, at, d); unlock;
//     r == REFUSED: release h (a seek came between)
//
// PURE, AND NOT LOCKED: it calls nothing but the release a flush is given,
// and allocates nothing (the caller hands it its slots), so
// tools/test_media_cores_host.sh drives it against a model. A caller whose
// producer and consumer are different threads holds one lock of its own
// across every call.
//
// ORDER. Entries come out in the order they went in, which for pictures is
// the order a decoder shows them in. The queue does not sort or judge
// times; what is due is os64/media_pace.h's question.
//
// FULL is `capacity` entries. A push to a full queue takes nothing and says
// so, and the producer parks until a take makes room.
//
// GENERATIONS. A flush empties the queue, hands every entry to the release
// it is given, and starts a new generation. A push names the generation its
// picture was made for, read BEFORE the producer let go of the lock to make
// it, and one made for an earlier generation is refused: the picture from
// before a seek is never shown after it. A refused handle is still the
// producer's to release.
//
// THE END. The producer says when its media has no more pictures this
// generation (os64_media_pictures_end); once what is queued has been taken,
// os64_media_pictures_finished is true. A flush clears it.

#include <stdbool.h>
#include <stdint.h>

#pragma GCC visibility push(default)

typedef struct {
    void *handle;           // the caller's
    int64_t at;             // shown from this output frame
    int64_t duration;       // for this many (0 when not known)
} os64_media_pictures_entry_t;

typedef struct {
    os64_media_pictures_entry_t *slot;  // the caller's: `capacity` of them
    uint32_t capacity;
    uint32_t head;          // the front entry's slot
    uint32_t count;
    uint32_t generation;
    bool ended;
} os64_media_pictures_t;

#define OS64_MEDIA_PICTURES_TAKEN    1
#define OS64_MEDIA_PICTURES_FULL     0    // nothing taken; push again after a take
#define OS64_MEDIA_PICTURES_REFUSED  (-1) // never taken: a generation not the queue's, a NULL
                                          // handle, or pushed after the end

// An empty queue over `slot` (`capacity` entries, which stay the caller's),
// generation 0.
void os64_media_pictures_init(os64_media_pictures_t *q, os64_media_pictures_entry_t *slot, uint32_t capacity);

uint32_t os64_media_pictures_count(const os64_media_pictures_t *q);
bool os64_media_pictures_full(const os64_media_pictures_t *q);
// The generation a push is made for now.
uint32_t os64_media_pictures_generation(const os64_media_pictures_t *q);

// One entry at the back, made for `generation`: OS64_MEDIA_PICTURES_*.
int os64_media_pictures_push(os64_media_pictures_t *q, uint32_t generation, void *handle, int64_t at,
                             int64_t duration);

// The entry `index` places from the front (0 is the front), or NULL when
// there are not that many. It stays in the queue.
const os64_media_pictures_entry_t *os64_media_pictures_peek(const os64_media_pictures_t *q, uint32_t index);
// The time of the back entry, the newest, or -1 when empty.
int64_t os64_media_pictures_newest(const os64_media_pictures_t *q);

// The front entry out into *out (which may be NULL when the caller already
// has it from a peek); false when empty.
bool os64_media_pictures_take(os64_media_pictures_t *q, os64_media_pictures_entry_t *out);

// Empty it, giving each entry's handle to `release` (NULL: none is called),
// front first; forget the end; begin the next generation, which it returns.
uint32_t os64_media_pictures_flush(os64_media_pictures_t *q, void (*release)(void *context, void *handle),
                                   void *context);

// No more will be pushed this generation.
void os64_media_pictures_end(os64_media_pictures_t *q);
bool os64_media_pictures_ended(const os64_media_pictures_t *q);
// Ended, and everything pushed has been taken.
bool os64_media_pictures_finished(const os64_media_pictures_t *q);

#pragma GCC visibility pop

#endif // OS64_MEDIA_PICTURES_H
