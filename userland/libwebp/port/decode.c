#include "webp/webp.h"
#include "src/webp/decode.h"
#include "src/dsp/dsp.h"
#include "src/dsp/lossless.h"
#include "src/dsp/yuv.h"
#include "os64/mem.h"
#include "os64/str.h"
#include "os64/proc.h"
#include "webp_license.h"
#ifdef OS64_WEBP_BENCH
#include "bench.h"
#endif

typedef union allocation allocation;
union allocation {
    struct { size_t charge; allocation *next, *prev; } block;
    max_align_t alignment;
};
typedef struct {
    size_t used, cap;
    allocation *blocks;
    os64_webp_status_t refusal;
#ifdef OS64_WEBP_BENCH
    webp_bench_profile_t *profile;
#endif
} decoder;
static unsigned owned, initialized;
static decoder *active;

/* Upstream allocation hooks have no context argument. The gate also protects
 * DSP guards, including private lazy InitGetCoeffs, throughout cleanup. */
static void acquire(void)
{
    while (__atomic_exchange_n(&owned, 1, __ATOMIC_ACQUIRE)) os64_sleep(1);
    if (!__atomic_load_n(&initialized, __ATOMIC_ACQUIRE)) {
        VP8DspInit(); VP8LDspInit(); VP8FiltersInit();
        WebPInitAlphaProcessing(); WebPInitUpsamplers(); WebPInitSamplers();
        WebPInitYUV444Converters(); WebPRescalerDspInit();
        __atomic_store_n(&initialized, 1, __ATOMIC_RELEASE);
    }
}
static void refuse(os64_webp_status_t status)
{
    if (active->refusal == OS64_WEBP_OK) active->refusal = status;
}
void *webp_port_malloc(size_t n)
{
    if (n > SIZE_MAX - sizeof(allocation) ||
        n + sizeof(allocation) > active->cap - active->used) {
        refuse(OS64_WEBP_LIMIT); return NULL;
    }
#ifdef OS64_WEBP_BENCH
    int64_t before = os64_micros();
#endif
    allocation *p = os64_malloc(n + sizeof(*p));
#ifdef OS64_WEBP_BENCH
    active->profile->allocate_us += os64_micros() - before;
#endif
    if (!p) { refuse(OS64_WEBP_NO_MEMORY); return NULL; }
    p->block.charge = n + sizeof(*p);
    p->block.next = active->blocks;
    p->block.prev = NULL;
    if (active->blocks) active->blocks->block.prev = p;
    active->blocks = p;
    active->used += p->block.charge;
    return p + 1;
}
void *webp_port_calloc(size_t count, size_t size)
{
    if (count && size > SIZE_MAX / count) { refuse(OS64_WEBP_LIMIT); return NULL; }
    void *p = webp_port_malloc(count * size);
    if (p) os64_memset(p, 0, count * size);
    return p;
}
void webp_port_free(void *ptr)
{
    if (!ptr) return;
    allocation *p = (allocation *)ptr - 1;
    if (p->block.prev) p->block.prev->block.next = p->block.next;
    else active->blocks = p->block.next;
    if (p->block.next) p->block.next->block.prev = p->block.prev;
    active->used -= p->block.charge;
#ifdef OS64_WEBP_BENCH
    int64_t before = os64_micros();
#endif
    os64_free(p);
#ifdef OS64_WEBP_BENCH
    active->profile->scratch_free_us += os64_micros() - before;
#endif
}
static uint32_t le24(const uint8_t *p)
{ return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16; }
static uint32_t le32(const uint8_t *p)
{ return le24(p) | (uint32_t)p[3] << 24; }
static int tag(const uint8_t *p, const char *s) { return !os64_memcmp(p, s, 4); }
typedef struct {
    size_t start, end;
    uint32_t width, height;
    int extended, lossless;
} framing;

/* Walk the entire declared extent, including chunks after the raster. Metadata
 * and unknown chunks may be interleaved, but reconstruction order matters.
 * Reserved fields and inactive ANIM data are ignored as the container spec
 * requires. Animation rejection validates outer framing, not frame bitstreams. */
static os64_webp_status_t frame(const uint8_t *data, size_t length, framing *f)
{
    uint64_t extent64 = (uint64_t)le32(data + 4) + 8;
    if (extent64 < 20 || extent64 > length) return OS64_WEBP_MALFORMED;
    size_t extent = (size_t)extent64;
    int raster = 0, alpha = 0, anim = 0, frames = 0;
    unsigned flags = 0;
    for (size_t at = 12; at < extent;) {
        if (extent - at < 8) return OS64_WEBP_MALFORMED;
        const uint8_t *chunk = data + at, *p = chunk + 8;
        size_t n = le32(chunk + 4), left = extent - at - 8;
        if (n > left || (n & 1) > left - n) return OS64_WEBP_MALFORMED;
        size_t end = at + 8 + n + (n & 1);
        if (at == 12 && !tag(chunk, "VP8X") && !tag(chunk, "VP8 ") && !tag(chunk, "VP8L"))
            return OS64_WEBP_MALFORMED;
        if (tag(chunk, "VP8X")) {
            if (at != 12 || n < 10) return OS64_WEBP_MALFORMED;
            f->extended = 1; flags = p[0];
            f->width = le24(p + 4) + 1; f->height = le24(p + 7) + 1;
            if ((uint64_t)f->width * f->height > UINT32_MAX) return OS64_WEBP_MALFORMED;
        } else if (tag(chunk, "ANIM")) {
            if (flags & 2) {
                if (anim || frames || raster || alpha || n != 6) return OS64_WEBP_MALFORMED;
                anim = 1;
            }
        } else if (tag(chunk, "ANMF")) {
            if (!(flags & 2) || !anim || n < 16) return OS64_WEBP_MALFORMED;
            uint64_t x = (uint64_t)le24(p) * 2, y = (uint64_t)le24(p + 3) * 2;
            if (x + le24(p + 6) + 1 > f->width || y + le24(p + 9) + 1 > f->height)
                return OS64_WEBP_MALFORMED;
            frames = 1;
        } else if (tag(chunk, "ALPH")) {
            if (!f->extended || (flags & 2) || alpha || raster || n < 2 || !(flags & 16))
                return OS64_WEBP_MALFORMED;
            alpha = 1; f->start = at;
        } else if (tag(chunk, "VP8 ") || tag(chunk, "VP8L")) {
            if (raster || (flags & 2)) return OS64_WEBP_MALFORMED;
            f->lossless = tag(chunk, "VP8L");
            if (alpha && f->lossless) return OS64_WEBP_MALFORMED;
            if (!f->lossless && f->extended && !!(flags & 16) != alpha)
                return OS64_WEBP_MALFORMED;
            if (!alpha) f->start = at;
            f->end = end; raster = 1;
        } else if (tag(chunk, "ICCP")) {
            if (!f->extended || alpha || raster || anim || frames) return OS64_WEBP_MALFORMED;
        } else if (tag(chunk, "EXIF") || tag(chunk, "XMP ")) {
            if (!f->extended) return OS64_WEBP_MALFORMED;
        }
        at = end;
    }
    if (flags & 2) return anim && frames ? OS64_WEBP_UNSUPPORTED : OS64_WEBP_MALFORMED;
    return raster ? OS64_WEBP_OK : OS64_WEBP_MALFORMED;
}
static os64_webp_status_t translate(VP8StatusCode status)
{
    if (status == VP8_STATUS_OK) return OS64_WEBP_OK;
    if (status == VP8_STATUS_OUT_OF_MEMORY) return OS64_WEBP_NO_MEMORY;
    if (status == VP8_STATUS_UNSUPPORTED_FEATURE) return OS64_WEBP_UNSUPPORTED;
    return OS64_WEBP_MALFORMED;
}
#ifdef OS64_WEBP_BENCH
os64_webp_status_t webp_bench_decode(const uint8_t *data, size_t length,
    uint64_t pixel_cap, size_t memory_cap, os64_webp_image_t *out,
    webp_bench_profile_t *profile)
#else
os64_webp_status_t os64_webp_decode(const uint8_t *data, size_t length,
    uint64_t pixel_cap, size_t memory_cap, os64_webp_image_t *out)
#endif
{
#ifdef OS64_WEBP_BENCH
    *profile = (webp_bench_profile_t){0};
#endif
    if (!out) return OS64_WEBP_BAD_ARGUMENT;
    *out = (os64_webp_image_t){0};
    if (!data) return OS64_WEBP_BAD_ARGUMENT;
    if (length < 12 || !tag(data, "RIFF") || !tag(data + 8, "WEBP")) return OS64_WEBP_NOT_WEBP;
    if (length > OS64_WEBP_INPUT_MAX) return OS64_WEBP_LIMIT;
    framing f = {0};
    os64_webp_status_t status = frame(data, length, &f);
    if (status != OS64_WEBP_OK) return status;
    if (!pixel_cap) pixel_cap = OS64_WEBP_PIXEL_CAP_DEFAULT;
    if (!memory_cap) memory_cap = OS64_WEBP_MEMORY_DEFAULT;
    decoder d = {.cap = memory_cap};
    WebPDecoderConfig config;
    uint32_t *pixels = NULL;
    size_t bytes = 0;
#ifdef OS64_WEBP_BENCH
    d.profile = profile;
    int64_t gate_start = os64_micros();
#endif
    acquire(); active = &d;
#ifdef OS64_WEBP_BENCH
    profile->gate_us = os64_micros() - gate_start;
#endif
    if (!WebPInitDecoderConfig(&config)) { status = OS64_WEBP_UNSUPPORTED; goto done; }
    /* After validating the public RIFF file, pass its reconstruction span to
     * upstream's internal ALPH+VP8 / VP8(L) reader. This borrows the input and
     * avoids interpreting ignored metadata or future VP8X extension fields. */
    const uint8_t *encoded = data + f.start;
    size_t encoded_size = f.end - f.start;
    status = translate(WebPGetFeatures(encoded, encoded_size, &config.input));
    if (status != OS64_WEBP_OK) goto done;
    uint32_t w = config.input.width, h = config.input.height;
    if (!w || !h || (f.extended && (f.width != w || f.height != h))) {
        status = OS64_WEBP_MALFORMED; goto done;
    }
    if (w > OS64_WEBP_DIM_MAX || h > OS64_WEBP_DIM_MAX || (uint64_t)w * h > pixel_cap ||
        (uint64_t)w * h > SIZE_MAX / 4) { status = OS64_WEBP_LIMIT; goto done; }
    bytes = (size_t)w * h * 4;
    if (bytes > d.cap) { status = OS64_WEBP_LIMIT; goto done; }
#ifdef OS64_WEBP_BENCH
    int64_t output_start = os64_micros();
#endif
    pixels = os64_malloc(bytes);
#ifdef OS64_WEBP_BENCH
    profile->allocate_us += os64_micros() - output_start;
#endif
    if (!pixels) { status = OS64_WEBP_NO_MEMORY; goto done; }
    d.used = bytes;
    config.output.colorspace = MODE_BGRA;
    config.output.is_external_memory = 1;
    config.output.u.RGBA.rgba = (uint8_t *)pixels;
    config.output.u.RGBA.size = bytes;
    config.output.u.RGBA.stride = (int)w * 4;
    config.options.no_fancy_upsampling = 0;
    config.options.use_threads = 0;
    status = translate(WebPDecode(encoded, encoded_size, &config));
    if (status == OS64_WEBP_OK && (config.output.width != (int)w || config.output.height != (int)h))
        status = OS64_WEBP_MALFORMED;
    WebPFreeDecBuffer(&config.output);
    if (status == OS64_WEBP_OK && d.refusal == OS64_WEBP_OK && !d.blocks)
        *out = (os64_webp_image_t){w, h, pixels};
done:
    if (d.refusal != OS64_WEBP_OK) status = d.refusal;
    /* Defensive cleanup also covers a future upstream failure path that leaves
     * allocations behind. Such a decode must not publish an image. */
    if (d.blocks) {
        if (status == OS64_WEBP_OK) status = OS64_WEBP_MALFORMED;
        while (d.blocks) webp_port_free(d.blocks + 1);
    }
    if (status != OS64_WEBP_OK) os64_free(pixels);
    active = NULL;
    __atomic_store_n(&owned, 0, __ATOMIC_RELEASE);
    return status;
}
void os64_webp_free(os64_webp_image_t *image)
{
    if (image) { os64_free(image->pixels); *image = (os64_webp_image_t){0}; }
}
const char *os64_webp_status_name(os64_webp_status_t status)
{
    switch (status) {
    case OS64_WEBP_OK: return "ok";
    case OS64_WEBP_NOT_WEBP: return "not WebP";
    case OS64_WEBP_MALFORMED: return "malformed WebP";
    case OS64_WEBP_UNSUPPORTED: return "unsupported WebP variant";
    case OS64_WEBP_LIMIT: return "WebP resource limit";
    case OS64_WEBP_NO_MEMORY: return "out of memory";
    case OS64_WEBP_BAD_ARGUMENT: return "invalid argument";
    default: return "unknown WebP status";
    }
}
const char *os64_webp_license(void) { return webp_license; }
