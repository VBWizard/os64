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

// THE UNSCALED EDGE. A conversion with no scaling does not write exactly
// the width it is given when that width is not a multiple of eight. From
// 4:2:0 or 4:2:2 at an even height it takes libswscale's unscaled
// converter, whose x86 form works in whole groups of eight pixels across
// (x86/yuv2rgb.c, YUV2RGB_LOOP): with no room in the pitch for the last
// group it leaves the row's last few pixels unwritten; with room, it
// writes past the width, and on the last row past the end of the caller's
// buffer. The accurate path (SWS_ACCURATE_RND) writes past the width too,
// by fewer. And an interlaced picture is converted field by field
// (libswscale/graph.c, get_field), so a field can be unscaled while the
// frame is not: 854x480 to 854x479 has two fields of 854x240. So every
// conversion that keeps the width, whatever its height, is made into
// `bounce`, the scaler's own buffer with room past the width and a row to
// spare, and the width of each row is copied to the caller. A conversion
// that changes the width writes exactly its width
// (tools/test_media_picture_host.sh checks both, interlaced included,
// against guard words, which ASan cannot do for writes FFmpeg's assembly
// makes).
// FAST IS FAST ONLY WHEN SHRINKING. Enlarging, libswscale's fast bilinear
// writes an x86 horizontal filter of its own and runs it
// (libswscale/utils.c, ff_init_hscaler_mmxext); with no mmap or mprotect
// in this build it writes that code into av_malloc's memory, which os64
// does not let execute, and the jump into it is a no-execute fault (the
// P5's first `avbench -x 2560x1440`). So OS64_MEDIA_SCALE_FAST asks for
// SWS_FAST_BILINEAR only for a picture wider than the destination, and
// enlarges with plain bilinear (tools/test_av_audit.py pins when the
// scaler writes code; libav/README.md § The x86 assembly).
struct os64_media_scaler {
    SwsContext *sws;
    AVFrame *canvas;
    bool fast;                       // OS64_MEDIA_SCALE_FAST: SWS_FAST_BILINEAR when shrinking
    uint32_t *bounce;                // NULL until a conversion keeps a width not a multiple of 8
    size_t bounce_pixels;
    int last;                        // OS64_MEDIA_SCALED_*: the last conversion's way
};

// Pixels between the bounce buffer's rows for `width`: room for the last
// group of eight and as much again.
static uint32_t bounce_pitch(uint32_t width)
{
    return ((width + 15) & ~15u) + 16;
}

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
    s->fast = (flags & OS64_MEDIA_SCALE_FAST) != 0;
    s->sws->flags = flags & OS64_MEDIA_SCALE_EXACT ? SWS_BILINEAR | SWS_BITEXACT | SWS_ACCURATE_RND : SWS_BILINEAR;
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
    uint32_t width = (uint32_t)s->canvas->width, height = (uint32_t)s->canvas->height;
    bool edge = source->width == (int)width && (width & 7) != 0;
    if (edge) {
        // THE UNSCALED EDGE, above.
        size_t need = (size_t)bounce_pitch(width) * (height + 1);
        if (need > s->bounce_pixels) {
            uint32_t *bigger = os64_malloc_aligned(need * 4, 64);
            if (bigger == NULL) return OS64_MEDIA_NO_MEMORY;
            if (s->bounce != NULL) os64_free(s->bounce);
            s->bounce = bigger;
            s->bounce_pixels = need;
        }
        s->canvas->data[0] = (uint8_t *)s->bounce;
        s->canvas->linesize[0] = (int)(bounce_pitch(width) * 4);
    } else {
        s->canvas->data[0] = (uint8_t *)pixels;
        s->canvas->linesize[0] = (int)pitch;
    }
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
    if (s->fast) {
        // FAST IS FAST ONLY WHEN SHRINKING, above. A change rebuilds the
        // scaler's graph, which it does whenever its options change.
        unsigned want = source->width > (int)width ? SWS_FAST_BILINEAR : SWS_BILINEAR;
        if (s->sws->flags != want) s->sws->flags = want;
    }
    int r = sws_scale_frame(s->sws, s->canvas, source);
    s->canvas->data[0] = NULL;
    s->last = source->width == (int)width && source->height == (int)height ? OS64_MEDIA_SCALED_UNSCALED
            : (s->sws->flags & SWS_FAST_BILINEAR) ? OS64_MEDIA_SCALED_FAST_BILINEAR
                                                   : OS64_MEDIA_SCALED_BILINEAR;
    if (r < 0) return r == AVERROR(ENOMEM) ? OS64_MEDIA_NO_MEMORY : OS64_MEDIA_UNSUPPORTED;
    if (edge)
        for (uint32_t y = 0; y < height; y++)
            os64_memcpy((uint8_t *)pixels + (size_t)y * pitch, s->bounce + (size_t)y * bounce_pitch(width),
                        (size_t)width * 4);
    return 0;
}

int os64_media_scaler_last(const os64_media_scaler_t *s)
{
    return s != NULL ? s->last : OS64_MEDIA_SCALED_NONE;
}

void os64_media_scaler_destroy(os64_media_scaler_t *s)
{
    if (s == NULL) return;
    sws_free_context(&s->sws);
    av_frame_free(&s->canvas);       // holds no buffer of FFmpeg's
    if (s->bounce != NULL) os64_free(s->bounce);
    os64_free(s);
}
