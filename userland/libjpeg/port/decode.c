#include "jpeg/jpeg.h"
#include "jerror.h"
#include "jmemsys.h"
#include "os64/mem.h"
#include "jpeg_license.h"

typedef struct {
    struct jpeg_decompress_struct jpeg;
    struct jpeg_error_mgr error;
    struct jpeg_source_mgr source;
    struct jpeg_progress_mgr progress;
    intptr_t escape[5];
    size_t used, cap;
    os64_jpeg_status_t status;
    uint32_t *pixels;
    unsigned char *row;
} decoder;

/* Both ends of this private GCC escape use the same scalar x86-64 target.
 * Keep the jump in a separate function: GCC's nonlocal-return builtin requires
 * a descendant frame. Mutable cleanup state lives in the heap-owned decoder. */
__attribute__((noreturn, noinline)) static void refuse(decoder *d, os64_jpeg_status_t status)
{
    if (d->status == OS64_JPEG_OK) d->status = status;
    __builtin_longjmp(d->escape, 1);
}
static void fatal(j_common_ptr common)
{
    decoder *d = common->client_data;
    int code = common->err->msg_code;
    os64_jpeg_status_t status = OS64_JPEG_MALFORMED;
    if (code == JERR_NOT_COMPILED || code == JERR_BAD_PRECISION ||
        code == JERR_SOF_UNSUPPORTED || code == JERR_ARITH_NOTIMPL)
        status = OS64_JPEG_UNSUPPORTED;
    else if (code == JERR_OUT_OF_MEMORY) status = OS64_JPEG_NO_MEMORY;
    else if (code == JERR_IMAGE_TOO_BIG) status = OS64_JPEG_LIMIT;
    refuse(d, status);
}
static void reset_error(j_common_ptr c)
{ c->err->num_warnings = 0; c->err->msg_code = 0; }
static void message(j_common_ptr common, int level)
{
    if (level < 0) refuse(common->client_data, OS64_JPEG_MALFORMED);
}
static void *allocate(decoder *d, size_t n)
{
    if (n > d->cap - d->used) refuse(d, OS64_JPEG_LIMIT);
    void *p = os64_malloc(n);
    if (!p) refuse(d, OS64_JPEG_NO_MEMORY);
    d->used += n;
    return p;
}
void *jpeg_get_small(j_common_ptr c, size_t n) { return allocate(c->client_data, n); }
void *jpeg_get_large(j_common_ptr c, size_t n) { return allocate(c->client_data, n); }
void jpeg_free_small(j_common_ptr c, void *p, size_t n)
{
    decoder *d = c->client_data;
    os64_free(p); d->used -= n;
}
void jpeg_free_large(j_common_ptr c, void *p, size_t n) { jpeg_free_small(c, p, n); }
size_t jpeg_mem_available(j_common_ptr c, size_t min, size_t max, size_t already)
{
    (void)min; (void)already;
    decoder *d = c->client_data;
    size_t left = d->cap - d->used;
    return left < max ? left : max;
}
void jpeg_open_backing_store(j_common_ptr c, backing_store_ptr store, long n)
{ (void)store; (void)n; refuse(c->client_data, OS64_JPEG_LIMIT); }
long jpeg_mem_init(j_common_ptr c) { (void)c; return 0; }
void jpeg_mem_term(j_common_ptr c) { (void)c; }

static void noop(j_decompress_ptr c) { (void)c; }
static boolean exhausted(j_decompress_ptr c) { refuse(c->client_data, OS64_JPEG_MALFORMED); }
static void skip(j_decompress_ptr c, long n)
{
    if (n <= 0) return;
    if ((size_t)n > c->src->bytes_in_buffer) refuse(c->client_data, OS64_JPEG_MALFORMED);
    c->src->bytes_in_buffer -= (size_t)n; c->src->next_input_byte += n;
}
static void progress(j_common_ptr c)
{
    decoder *d = c->client_data;
    if (d->jpeg.input_scan_number > 256) refuse(d, OS64_JPEG_LIMIT);
}
static unsigned read16(const uint8_t *p, int little)
{ return little ? p[0] | (unsigned)p[1] << 8 : (unsigned)p[0] << 8 | p[1]; }
static uint32_t read32(const uint8_t *p, int little)
{
    return little ? read16(p, 1) | (uint32_t)read16(p + 2, 1) << 16 :
        (uint32_t)read16(p, 0) << 16 | read16(p + 2, 0);
}
/* Only IFD0's inline SHORT orientation is needed; no TIFF pointer traversal. */
static unsigned orientation(decoder *d, const uint8_t *data, size_t length)
{
    size_t at = 2;
    unsigned result = 1;
    int seen = 0;
    while (at < length) {
        if (data[at++] != 255) refuse(d, OS64_JPEG_MALFORMED);
        while (at < length && data[at] == 255) at++;
        if (at == length) refuse(d, OS64_JPEG_MALFORMED);
        unsigned marker = data[at++];
        if (marker == 0xda || marker == 0xd9) break;
        if (marker == 1 || (marker >= 0xd0 && marker <= 0xd7)) continue;
        if (length - at < 2) refuse(d, OS64_JPEG_MALFORMED);
        size_t n = read16(data + at, 0);
        if (n < 2 || n > length - at) refuse(d, OS64_JPEG_MALFORMED);
        const uint8_t *p = data + at + 2;
        size_t available = n - 2;
        if (marker == 0xe1 && available >= 6 && p[0]=='E' && p[1]=='x' && p[2]=='i' && p[3]=='f' && !p[4] && !p[5]) {
            p += 6; available -= 6;
            if (available < 8) refuse(d, OS64_JPEG_MALFORMED);
            int little = p[0] == 'I' && p[1] == 'I';
            if ((!little && !(p[0] == 'M' && p[1] == 'M')) || read16(p + 2, little) != 42)
                refuse(d, OS64_JPEG_MALFORMED);
            size_t offset = read32(p + 4, little);
            if (offset < 8 || offset > available || available - offset < 2)
                refuse(d, OS64_JPEG_MALFORMED);
            size_t count = read16(p + offset, little); offset += 2;
            if (available - offset < 4 || count > (available - offset - 4) / 12)
                refuse(d, OS64_JPEG_MALFORMED);
            for (size_t i = 0; i < count; i++) {
                const uint8_t *e = p + offset + i * 12;
                if (read16(e, little) != 0x112) continue;
                unsigned value = read16(e + 8, little);
                if (seen || read16(e + 2, little) != 3 || read32(e + 4, little) != 1 || value < 1 || value > 8)
                    refuse(d, OS64_JPEG_MALFORMED);
                result = value; seen = 1;
            }
        }
        at += n;
    }
    return result;
}

os64_jpeg_status_t os64_jpeg_decode(const uint8_t *data, size_t length,
    uint64_t pixel_cap, size_t memory_cap, os64_jpeg_image_t *out)
{
    if (!out) return OS64_JPEG_BAD_ARGUMENT;
    *out = (os64_jpeg_image_t){0};
    if (!data) return OS64_JPEG_BAD_ARGUMENT;
    if (length < 2 || data[0] != 255 || data[1] != 216) return OS64_JPEG_NOT_JPEG;
    if (length > OS64_JPEG_INPUT_MAX) return OS64_JPEG_LIMIT;
    if (!pixel_cap) pixel_cap = OS64_JPEG_PIXEL_CAP_DEFAULT;
    if (!memory_cap) memory_cap = OS64_JPEG_MEMORY_DEFAULT;
    if (memory_cap < sizeof(decoder)) return OS64_JPEG_LIMIT;
    decoder *d = os64_malloc(sizeof *d);
    if (!d) return OS64_JPEG_NO_MEMORY;
    os64_memset(d, 0, sizeof *d);
    d->cap = memory_cap; d->used = sizeof *d;
    d->jpeg.client_data = d;
    d->error.error_exit = fatal; d->error.emit_message = message;
    d->error.reset_error_mgr = reset_error;
    d->jpeg.err = &d->error;
    if (!__builtin_setjmp(d->escape)) {
        unsigned turn = orientation(d, data, length);
        jpeg_create_decompress(&d->jpeg);
        d->source = (struct jpeg_source_mgr){data, length, noop, exhausted, skip, jpeg_resync_to_restart, noop};
        d->jpeg.src = &d->source;
        d->progress.progress_monitor = progress; d->jpeg.progress = &d->progress;
        if (jpeg_read_header(&d->jpeg, TRUE) != JPEG_HEADER_OK) refuse(d, OS64_JPEG_MALFORMED);
        uint32_t w = d->jpeg.image_width, h = d->jpeg.image_height;
        if (!w || !h) refuse(d, OS64_JPEG_MALFORMED);
        if (w > OS64_JPEG_DIM_MAX || h > OS64_JPEG_DIM_MAX || (uint64_t)w * h > pixel_cap)
            refuse(d, OS64_JPEG_LIMIT);
        if (d->jpeg.data_precision != 8 || d->jpeg.arith_code || d->jpeg.num_components > 4)
            refuse(d, OS64_JPEG_UNSUPPORTED);
        int cmyk = d->jpeg.jpeg_color_space == JCS_CMYK || d->jpeg.jpeg_color_space == JCS_YCCK;
        d->jpeg.out_color_space = cmyk ? JCS_CMYK : JCS_EXT_BGRA;
        d->jpeg.dct_method = JDCT_ISLOW;
        d->pixels = allocate(d, (size_t)w * h * 4);
        d->row = allocate(d, (size_t)w * 4);
        if (!jpeg_start_decompress(&d->jpeg)) refuse(d, OS64_JPEG_MALFORMED);
        uint32_t ow = turn >= 5 ? h : w, oh = turn >= 5 ? w : h;
        for (uint32_t y = 0; y < h; y++) {
            JSAMPROW row = d->row;
            if (jpeg_read_scanlines(&d->jpeg, &row, 1) != 1) refuse(d, OS64_JPEG_MALFORMED);
            for (uint32_t x = 0; x < w; x++) {
                const uint8_t *p = row + x * 4;
                uint32_t color;
                if (cmyk) {
                    unsigned r, g, b;
                    if (d->jpeg.saw_Adobe_marker) {
                        r = (p[0] * p[3] + 127) / 255; g = (p[1] * p[3] + 127) / 255; b = (p[2] * p[3] + 127) / 255;
                    } else {
                        r = ((255-p[0]) * (255-p[3]) + 127) / 255;
                        g = ((255-p[1]) * (255-p[3]) + 127) / 255;
                        b = ((255-p[2]) * (255-p[3]) + 127) / 255;
                    }
                    color = 0xff000000u | r << 16 | g << 8 | b;
                } else color = 0xff000000u | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
                uint32_t dx = x, dy = y;
                switch (turn) {
                case 2: dx=w-1-x; break;
                case 3: dx=w-1-x; dy=h-1-y; break;
                case 4: dy=h-1-y; break;
                case 5: dx=y; dy=x; break;
                case 6: dx=h-1-y; dy=x; break;
                case 7: dx=h-1-y; dy=w-1-x; break;
                case 8: dx=y; dy=w-1-x; break;
                }
                d->pixels[(size_t)dy * ow + dx] = color;
            }
        }
        if (!jpeg_finish_decompress(&d->jpeg)) refuse(d, OS64_JPEG_MALFORMED);
        *out = (os64_jpeg_image_t){ow, oh, d->pixels}; d->pixels = NULL;
    }
    jpeg_destroy_decompress(&d->jpeg);
    os64_free(d->row); os64_free(d->pixels);
    os64_jpeg_status_t status = d->status;
    os64_free(d);
    return status;
}
void os64_jpeg_free(os64_jpeg_image_t *image)
{ if (image) { os64_free(image->pixels); *image = (os64_jpeg_image_t){0}; } }
const char *os64_jpeg_status_name(os64_jpeg_status_t status)
{
    static const char *const names[] = {"ok", "not JPEG", "malformed JPEG", "unsupported JPEG", "JPEG resource limit", "out of memory", "bad argument"};
    return (unsigned)status < sizeof names / sizeof *names ? names[status] : "unknown JPEG status";
}
const char *os64_jpeg_license(void) { return jpeg_license_text; }

/* These entry points close upstream's runtime precision branches. The public
 * wrapper refuses non-8-bit input before starting the output pipeline. */
void j12init_color_deconverter(j_decompress_ptr c) { refuse(c->client_data, OS64_JPEG_UNSUPPORTED); }
void j12init_upsampler(j_decompress_ptr c) { refuse(c->client_data, OS64_JPEG_UNSUPPORTED); }
void j12init_merged_upsampler(j_decompress_ptr c) { refuse(c->client_data, OS64_JPEG_UNSUPPORTED); }
void j12init_inverse_dct(j_decompress_ptr c) { refuse(c->client_data, OS64_JPEG_UNSUPPORTED); }
void j12init_d_coef_controller(j_decompress_ptr c, boolean full)
{ (void)full; refuse(c->client_data, OS64_JPEG_UNSUPPORTED); }
void j12init_d_main_controller(j_decompress_ptr c, boolean full)
{ (void)full; refuse(c->client_data, OS64_JPEG_UNSUPPORTED); }
void j12init_d_post_controller(j_decompress_ptr c, boolean full)
{ (void)full; refuse(c->client_data, OS64_JPEG_UNSUPPORTED); }
