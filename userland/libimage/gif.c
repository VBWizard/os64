// GIF87a/89a first-raster decoding. Block framing is checked before allocating
// output; drawing, animation and disposal are consumers' concerns.
#include "gif.h"
#include "os64/os64.h"
#include "os64/mem.h"

#define GIF_PIXEL_CAP (16u * 1024u * 1024u)
#define GIF_CODES 4096u

typedef struct {
    const uint8_t *data;
    size_t len, at;
} gif_reader_t;

typedef struct {
    uint32_t left, top, width, height;
    uint32_t palette[256];
    unsigned colors, minimum;
    int transparent;
    bool interlaced;
    size_t start, end;
} gif_frame_t;

static const uint8_t *take(gif_reader_t *r, size_t count)
{
    if (count > r->len - r->at)
        return NULL;
    const uint8_t *p = r->data + r->at;
    r->at += count;
    return p;
}

static uint32_t le16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static bool subblocks(gif_reader_t *r)
{
    for (;;) {
        const uint8_t *p = take(r, 1);
        if (p == NULL)
            return false;
        if (*p == 0)
            return true;
        if (take(r, *p) == NULL)
            return false;
    }
}

static bool palette(gif_reader_t *r, unsigned count, uint32_t *colors)
{
    const uint8_t *p = take(r, count * 3u);
    if (p == NULL)
        return false;
    for (unsigned i = 0; i < count; i++, p += 3)
        colors[i] = 0xff000000u | ((uint32_t)p[0] << 16) |
                    ((uint32_t)p[1] << 8) | p[2];
    return true;
}

// Save the first raster and walk subsequent block boundaries to the trailer.
// Later frames' compressed codes are not decoded or semantically validated.
static os64_image_status_t parse(gif_reader_t *r, gif_frame_t *first,
                                  uint32_t *width, uint32_t *height)
{
    const uint8_t *p = take(r, 13);
    if (p == NULL)
        return OS64_IMAGE_MALFORMED;
    *width = le16(p + 6);
    *height = le16(p + 8);
    if (*width == 0 || *height == 0)
        return OS64_IMAGE_MALFORMED;
    if (*width > OS64_IMAGE_DIM_MAX || *height > OS64_IMAGE_DIM_MAX ||
        (uint64_t)*width * *height > GIF_PIXEL_CAP)
        return OS64_IMAGE_LIMIT;
    uint32_t global[256];
    unsigned global_count = (p[10] & 0x80) ? 2u << (p[10] & 7) : 0;
    if (!palette(r, global_count, global))
        return OS64_IMAGE_MALFORMED;
    bool found = false, pending_control = false;
    int transparent = -1;
    for (;;) {
        p = take(r, 1);
        if (p == NULL)
            return OS64_IMAGE_MALFORMED;
        if (*p == 0x3b)
            return found && !pending_control && r->at == r->len ? OS64_IMAGE_OK : OS64_IMAGE_MALFORMED;
        if (*p == 0x21) {
            p = take(r, 1);
            if (p == NULL)
                return OS64_IMAGE_MALFORMED;
            unsigned label = *p;
            if (label == 0xf9) {
                p = take(r, 6);
                if (p == NULL || pending_control || p[0] != 4 || p[5] != 0 ||
                    (p[1] & 0xe0) || ((p[1] >> 2) & 7) > 3)
                    return OS64_IMAGE_MALFORMED;
                transparent = (p[1] & 1) ? p[4] : -1;
                pending_control = true;
            } else {
                // Known extensions have a fixed header before their data
                // sub-blocks. Unknown extensions retain sub-block framing.
                if (label == 0x01 || label == 0xff) {
                    p = take(r, 1);
                    if (p == NULL || *p != (label == 0x01 ? 12 : 11) ||
                        take(r, *p) == NULL)
                        return OS64_IMAGE_MALFORMED;
                }
                if (!subblocks(r))
                    return OS64_IMAGE_MALFORMED;
                // Rendering extensions consume the pending GCE, even when
                // skipped. Comments and application data do not consume it.
                if (label < 0x80) {
                    transparent = -1;
                    pending_control = false;
                }
            }
            continue;
        }
        if (*p != 0x2c)
            return OS64_IMAGE_MALFORMED;
        p = take(r, 9);
        if (p == NULL)
            return OS64_IMAGE_MALFORMED;
        gif_frame_t frame = {0};
        frame.left = le16(p);
        frame.top = le16(p + 2);
        frame.width = le16(p + 4);
        frame.height = le16(p + 6);
        if (frame.width == 0 || frame.height == 0 || (p[8] & 0x18) ||
            frame.left + frame.width > *width || frame.top + frame.height > *height)
            return OS64_IMAGE_MALFORMED;
        frame.interlaced = (p[8] & 0x40) != 0;
        frame.transparent = transparent;
        transparent = -1;
        pending_control = false;
        if (p[8] & 0x80) {
            frame.colors = 2u << (p[8] & 7);
            if (!palette(r, frame.colors, frame.palette))
                return OS64_IMAGE_MALFORMED;
        } else {
            frame.colors = global_count;
            os64_memcpy(frame.palette, global, global_count * sizeof(uint32_t));
        }
        p = take(r, 1);
        if (p == NULL || *p < 2 || *p > 8)
            return OS64_IMAGE_MALFORMED;
        frame.minimum = *p;
        frame.start = r->at;
        if (!subblocks(r))
            return OS64_IMAGE_MALFORMED;
        frame.end = r->at;
        if (!found) {
            *first = frame;
            found = true;
        }
    }
}

typedef struct {
    gif_reader_t reader;
    unsigned remaining, bits;
    uint32_t buffer;
} gif_bits_t;

static bool code_read(gif_bits_t *b, unsigned width, unsigned *code)
{
    while (b->bits < width) {
        if (b->remaining == 0) {
            const uint8_t *p = take(&b->reader, 1);
            if (p == NULL || *p == 0)
                return false;
            b->remaining = *p;
        }
        const uint8_t *p = take(&b->reader, 1);
        if (p == NULL)
            return false;
        b->remaining--;
        b->buffer |= (uint32_t)*p << b->bits;
        b->bits += 8;
    }
    *code = b->buffer & ((1u << width) - 1);
    b->buffer >>= width;
    b->bits -= width;
    return true;
}

typedef struct {
    uint16_t prefix[GIF_CODES];
    uint8_t suffix[GIF_CODES], stack[GIF_CODES];
} gif_dictionary_t;

static bool expand(const uint8_t *data, const gif_frame_t *f, uint32_t stride,
                   uint32_t *pixels, gif_dictionary_t *dict)
{
    gif_bits_t bits = {.reader = {data, f->end, f->start}};
    unsigned clear = 1u << f->minimum, end = clear + 1;
    unsigned next = end + 1, width = f->minimum + 1;
    int previous = -1;
    unsigned first = 0, x = 0, y = 0, pass = 0;
    size_t written = 0, count = (size_t)f->width * f->height;
    const unsigned starts[] = {0, 4, 2, 1};
    const unsigned steps[] = {8, 8, 4, 2};
    for (;;) {
        unsigned code;
        if (!code_read(&bits, width, &code))
            return false;
        if (code == clear) {
            next = end + 1;
            width = f->minimum + 1;
            previous = -1;
            continue;
        }
        if (code == end)
            return written == count;
        unsigned incoming = code, used = 0;
        if (previous < 0) {
            if (code >= clear)
                return false;
        } else if (code == next && next < GIF_CODES) {
            // KwKwK: the not-yet-inserted entry is the previous string
            // followed by its own first byte. The stack unwinds in reverse.
            dict->stack[used++] = (uint8_t)first;
            code = (unsigned)previous;
        } else if (code >= next) {
            return false;
        }
        while (code >= clear) {
            if (code <= end || code >= next || used >= GIF_CODES - 1)
                return false;
            dict->stack[used++] = dict->suffix[code];
            // Entries refer to earlier entries, bounding chain traversal.
            if (dict->prefix[code] >= code)
                return false;
            code = dict->prefix[code];
        }
        first = code;
        dict->stack[used++] = (uint8_t)first;
        if (used > count - written)
            return false;
        written += used;
        while (used != 0) {
            unsigned index = dict->stack[--used];
            if (index >= f->colors)
                return false;
            uint32_t color = f->palette[index];
            if ((int)index == f->transparent)
                color &= 0x00ffffffu;
            pixels[(size_t)(f->top + y) * stride + f->left + x] = color;
            if (++x == f->width) {
                x = 0;
                if (!f->interlaced) {
                    y++;
                } else {
                    y += steps[pass];
                    while (y >= f->height && pass < 3)
                        y = starts[++pass];
                }
            }
        }
        if (previous >= 0 && next < GIF_CODES) {
            dict->prefix[next] = (uint16_t)previous;
            dict->suffix[next++] = (uint8_t)first;
            if (next == (1u << width) && width < 12)
                width++;
        }
        // At 4096 entries, keep using the table at 12 bits until Clear;
        // deferred clear is legal (GIF89a cover sheet and Appendix F).
        previous = (int)incoming;
    }
}

os64_image_status_t image_decode_gif(const uint8_t *data, size_t len,
                                      os64_image_t *out)
{
    gif_reader_t reader = {data, len, 0};
    gif_frame_t frame = {0};
    uint32_t width, height;
    os64_image_status_t status = parse(&reader, &frame, &width, &height);
    if (status != OS64_IMAGE_OK)
        return status;
    // GIF can inherit a palette from an earlier stream. This stateless
    // decoder cannot supply one, but the file is not necessarily malformed.
    if (frame.colors == 0)
        return OS64_IMAGE_UNSUPPORTED;
    uint32_t *pixels = os64_malloc((size_t)width * height * sizeof(uint32_t));
    if (pixels == NULL)
        return OS64_IMAGE_NO_MEMORY;
    gif_dictionary_t *dict = os64_malloc(sizeof(*dict));
    if (dict == NULL) {
        os64_free(pixels);
        return OS64_IMAGE_NO_MEMORY;
    }
    // The browser-facing canvas is transparent outside the first raster;
    // the logical screen's background color does not fill those pixels.
    os64_memset(pixels, 0, (size_t)width * height * sizeof(uint32_t));
    bool valid = expand(data, &frame, width, pixels, dict);
    os64_free(dict);
    if (!valid) {
        os64_free(pixels);
        return OS64_IMAGE_MALFORMED;
    }
    *out = (os64_image_t){width, height, pixels};
    return OS64_IMAGE_OK;
}
