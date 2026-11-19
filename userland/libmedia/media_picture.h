#ifndef OS64_MEDIA_PICTURE_H
#define OS64_MEDIA_PICTURE_H

// media_picture.h — libmedia's own view of a picture, shared by media.c
// (which makes and returns them) and media_scale.c (which reads them).
// Never installed: a caller sees only os64_media_picture_t.

#include "os64/media.h"

#include "libavutil/frame.h"

typedef struct media_picture {
    os64_media_picture_t pub;       // first: the caller's handle points at it
    AVFrame *frame;                 // the decoder's frame, one reference of our own
    struct media_picture *next;     // on the media's released list
} media_picture_t;

static inline const AVFrame *media_picture_frame(const os64_media_picture_t *p)
{
    return ((const media_picture_t *)p)->frame;
}

#endif // OS64_MEDIA_PICTURE_H
