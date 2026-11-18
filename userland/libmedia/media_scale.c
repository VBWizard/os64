// media_scale.c — os64/media.h's scaler: a decoded picture into a window's
// XRGB8888 pixels, converted and scaled in one step by FFmpeg's libswscale.
//
// A scaler belongs to the thread converting, which is usually not the
// decoding thread, so it only READS a picture: sws_scale_frame on a
// DYNAMIC context (never sws_init_context, the legacy API, whose
// sws_frame_start takes a reference to the source) with a destination
// whose buffers are the caller's takes no reference to the source and
// drops none (libswscale/swscale.c, sws_scale_frame), so no FFmpeg
// reference count or pool is touched off the decoding thread (media.c
// § THREADS). The colour rule was written into the picture's frame on the
// decoding thread when it was made. tools/test_av_audit.py holds this
// file to it: what os64_media_scaler_convert can reach of libav.so is the
// scaler's own calls and nothing that frees.
//
// The destination is an AVFrame from av_frame_alloc (AVFrame's size is not
// FFmpeg's ABI, so a frame FFmpeg reads is always one it allocated) that
// holds no buffer of FFmpeg's: its plane is pointed at the caller's pixels
// at each conversion.

#include "os64/media.h"

#include "os64/mem.h"

#include "libavutil/frame.h"
#include "libswscale/swscale.h"

#include "media_picture.h"

struct os64_media_scaler {
    SwsContext *sws;
    AVFrame *canvas;
};

int os64_media_scaler_create(uint32_t dst_width, uint32_t dst_height, uint32_t flags, os64_media_scaler_t **out)
{
    if (out == NULL || dst_width == 0 || dst_height == 0 || dst_width > 16384 || dst_height > 16384 ||
        (flags & ~(OS64_MEDIA_SCALE_EXACT | OS64_MEDIA_SCALE_FAST)) != 0 ||
        (flags & OS64_MEDIA_SCALE_EXACT && flags & OS64_MEDIA_SCALE_FAST))
        return OS64_MEDIA_BAD_ARGS;
    *out = NULL;
    os64_media_scaler_t *s = os64_calloc(1, sizeof(*s));
    if (s == NULL) return OS64_MEDIA_NO_MEMORY;
    s->sws = sws_alloc_context();
    s->canvas = av_frame_alloc();
    if (s->sws == NULL || s->canvas == NULL) {
        os64_media_scaler_destroy(s);
        return OS64_MEDIA_NO_MEMORY;
    }
    s->sws->flags = flags & OS64_MEDIA_SCALE_FAST ? SWS_FAST_BILINEAR
                  : flags & OS64_MEDIA_SCALE_EXACT ? SWS_BILINEAR | SWS_BITEXACT | SWS_ACCURATE_RND
                  : SWS_BILINEAR;
    s->sws->threads = 1;                 // starts no threads (libav is built without them)
    // BGRA in memory is XRGB8888 in a 32-bit word, and a source without
    // alpha gives an opaque 0xFF X byte.
    s->canvas->format = AV_PIX_FMT_BGRA;
    s->canvas->width = (int)dst_width;
    s->canvas->height = (int)dst_height;
    s->canvas->colorspace = AVCOL_SPC_RGB;
    s->canvas->color_range = AVCOL_RANGE_JPEG;
    *out = s;
    return 0;
}

int os64_media_scaler_convert(os64_media_scaler_t *s, const os64_media_picture_t *p, uint32_t *pixels,
                              uint32_t pitch)
{
    if (s == NULL || p == NULL || pixels == NULL || pitch < (uint32_t)s->canvas->width * 4 || pitch > INT32_MAX)
        return OS64_MEDIA_BAD_ARGS;
    const AVFrame *source = media_picture_frame(p);
    s->canvas->data[0] = (uint8_t *)pixels;
    s->canvas->linesize[0] = (int)pitch;
    // Primaries and transfer the same on both sides, so the scaler converts
    // neither (left unspecified, FFmpeg maps a wide gamut, PQ or HLG to
    // BT.709: libswscale/format.c, ff_infer_colors).
    s->canvas->color_primaries = source->color_primaries;
    s->canvas->color_trc = source->color_trc;
    // An interlaced picture (H.264 says so per frame) is converted field by
    // field into the destination's two fields; the scaler refuses a source
    // and a destination that disagree (libswscale/swscale.c,
    // sws_frame_setup), and it does not deinterlace.
    s->canvas->flags = (s->canvas->flags & ~AV_FRAME_FLAG_INTERLACED) | (source->flags & AV_FRAME_FLAG_INTERLACED);
    int r = sws_scale_frame(s->sws, s->canvas, source);
    s->canvas->data[0] = NULL;
    if (r >= 0) return 0;
    return r == AVERROR(ENOMEM) ? OS64_MEDIA_NO_MEMORY : OS64_MEDIA_UNSUPPORTED;
}

void os64_media_scaler_destroy(os64_media_scaler_t *s)
{
    if (s == NULL) return;
    sws_free_context(&s->sws);
    av_frame_free(&s->canvas);       // holds no buffer of FFmpeg's
    os64_free(s);
}
