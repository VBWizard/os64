// GIF raster decoding and incremental image sequences. Framing is checked
// before allocation; playback timing and window drawing belong to consumers.
#include "gif.h"
#include "image/sequence.h"
#include "os64/slurp.h"
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
    unsigned colors, minimum, disposal, delay_ms;
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

typedef struct {
    gif_frame_t first;
    uint32_t width, height, plays;
    size_t count, max_raster, max_restore;
} gif_scan_t;

// The same framing walk serves first-picture decode and the sequence index.
// Animation-only metadata is interpreted when indexing; first-picture calls
// keep their existing treatment of application and Plain Text extensions.
static os64_image_status_t parse(gif_reader_t *r, gif_scan_t *scan,
                                  gif_frame_t *frames, size_t capacity,
                                  bool animation)
{
    *scan = (gif_scan_t){.plays = 1};
    uint32_t *width = &scan->width, *height = &scan->height;
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
    unsigned disposal = 0, delay_ms = 0;
    bool loop_seen = false;
    for (;;) {
        p = take(r, 1);
        if (p == NULL)
            return OS64_IMAGE_MALFORMED;
        if (*p == 0x3b) {
            // Tolerate an unused trailing control from legacy encoders. Its
            // settings target a following graphic, not the preceding frame.
            return found && r->at == r->len ? OS64_IMAGE_OK : OS64_IMAGE_MALFORMED;
        }
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
                if (animation && (p[1] & 2))
                    return OS64_IMAGE_UNSUPPORTED; // Interactive GIF waits need a consumer policy.
                disposal = (p[1] >> 2) & 7;
                delay_ms = le16(p + 2) * 10u;
                transparent = (p[1] & 1) ? p[4] : -1;
                pending_control = true;
            } else {
                // Plain Text renders graphics with its own timing/disposal.
                // First-picture decoding skips it; animation cannot pretend it
                // was a picture-free comment without changing the sequence.
                if (animation && label == 0x01)
                    return OS64_IMAGE_UNSUPPORTED;
                if (label == 0x01 || label == 0xff) {
                    p = take(r, 1);
                    if (p == NULL || *p != (label == 0x01 ? 12 : 11))
                        return OS64_IMAGE_MALFORMED;
                    unsigned header_size = *p;
                    p = take(r, header_size);
                    if (p == NULL)
                        return OS64_IMAGE_MALFORMED;
                    if (animation && label == 0xff &&
                        (!os64_memcmp(p, "NETSCAPE2.0", 11) ||
                         !os64_memcmp(p, "ANIMEXTS1.0", 11))) {
                        p = take(r, 4);
                        if (p == NULL || p[0] != 3 || p[1] != 1 || loop_seen)
                            return OS64_IMAGE_MALFORMED;
                        unsigned repeats = le16(p + 2);
                        scan->plays = repeats ? repeats + 1 : 0;
                        loop_seen = true;
                    }
                }
                if (!subblocks(r))
                    return OS64_IMAGE_MALFORMED;
                // Rendering extensions consume the pending GCE, even when
                // skipped. Comments and application data do not consume it.
                if (label < 0x80) {
                    transparent = -1;
                    disposal = delay_ms = 0;
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
        frame.disposal = disposal;
        frame.delay_ms = delay_ms;
        transparent = -1;
        disposal = delay_ms = 0;
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
            scan->first = frame;
            found = true;
        }
        size_t pixels = (size_t)frame.width * frame.height;
        if (pixels > scan->max_raster) scan->max_raster = pixels;
        if (frame.disposal == 3 && pixels > scan->max_restore) scan->max_restore = pixels;
        if (frames) {
            if (scan->count >= capacity) return OS64_IMAGE_MALFORMED;
            frames[scan->count] = frame;
        }
        scan->count++;
        if (animation && scan->count > 4096) return OS64_IMAGE_LIMIT;
        if (animation && !frame.colors) return OS64_IMAGE_UNSUPPORTED;
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
                   uint32_t *pixels, uint8_t *indices, gif_dictionary_t *dict)
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
            if (indices)
                indices[(size_t)y * f->width + x] = (uint8_t)index;
            else
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
    gif_scan_t scan;
    os64_image_status_t status = parse(&reader, &scan, NULL, 0, false);
    if (status != OS64_IMAGE_OK)
        return status;
    gif_frame_t frame = scan.first;
    uint32_t width = scan.width, height = scan.height;
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
    bool valid = expand(data, &frame, width, pixels, NULL, dict);
    os64_free(dict);
    if (!valid) {
        os64_free(pixels);
        return OS64_IMAGE_MALFORMED;
    }
    *out = (os64_image_t){width, height, pixels};
    return OS64_IMAGE_OK;
}

#define GIF_SEQUENCE_MEMORY (128u * 1024u * 1024u)

struct os64_image_sequence {
    os64_image_frame_t view;
    uint32_t *canvas, *restore;
    uint8_t *data, *indices;
    gif_frame_t *frames;
    gif_dictionary_t *dict;
    uint32_t pass;
};

void os64_image_sequence_free(os64_image_sequence_t *seq)
{
    if (!seq) return;
    os64_free(seq->canvas);
    os64_free(seq->restore);
    os64_free(seq->data);
    os64_free(seq->indices);
    os64_free(seq->frames);
    os64_free(seq->dict);
    os64_free(seq);
}

const os64_image_frame_t *os64_image_sequence_frame(const os64_image_sequence_t *seq)
{
    return seq ? &seq->view : NULL;
}

static os64_image_status_t sequence_render(os64_image_sequence_t *seq,
                                            uint32_t index, bool reset)
{
    const gif_frame_t *f = &seq->frames[index];
    // Decode into indices before touching the published canvas or its saved
    // rectangle. A bad later raster leaves the last good picture intact.
    if (!expand(seq->data, f, 0, NULL, seq->indices, seq->dict))
        return OS64_IMAGE_MALFORMED;
    size_t stride = seq->view.width;
    if (reset) {
        os64_memset(seq->canvas, 0, stride * seq->view.height * sizeof(uint32_t));
    } else {
        const gif_frame_t *old = &seq->frames[seq->view.index];
        for (uint32_t y = 0; y < old->height; y++) {
            uint32_t *row = seq->canvas + (old->top + y) * stride + old->left;
            if (old->disposal == 2)
                os64_memset(row, 0, old->width * sizeof(uint32_t));
            else if (old->disposal == 3)
                os64_memcpy(row, seq->restore + (size_t)y * old->width,
                            old->width * sizeof(uint32_t));
        }
    }
    // Restore-to-previous remembers the canvas BEFORE this frame is drawn.
    // Only the affected rectangle is saved, in a preallocated maximum-size buffer.
    for (uint32_t y = 0; y < f->height; y++) {
        uint32_t *row = seq->canvas + (f->top + y) * stride + f->left;
        if (f->disposal == 3)
            os64_memcpy(seq->restore + (size_t)y * f->width, row,
                        f->width * sizeof(uint32_t));
        for (uint32_t x = 0; x < f->width; x++) {
            unsigned color = seq->indices[(size_t)y * f->width + x];
            if ((int)color != f->transparent)
                row[x] = f->palette[color];
        }
    }
    seq->view.index = index;
    seq->view.delay_ms = f->delay_ms;
    return OS64_IMAGE_OK;
}

os64_image_status_t os64_image_sequence_rewind(os64_image_sequence_t *seq)
{
    if (!seq) return OS64_IMAGE_MALFORMED;
    os64_image_status_t st = seq->frames ? sequence_render(seq, 0, true) : OS64_IMAGE_OK;
    if (st == OS64_IMAGE_OK) seq->pass = 1;
    return st;
}

os64_image_status_t os64_image_sequence_next(os64_image_sequence_t *seq)
{
    if (!seq) return OS64_IMAGE_MALFORMED;
    if (!seq->frames || seq->view.frame_count == 1) return OS64_IMAGE_END;
    uint32_t index = seq->view.index + 1;
    bool reset = index == seq->view.frame_count;
    if (reset && seq->view.play_count && seq->pass >= seq->view.play_count)
        return OS64_IMAGE_END;
    os64_image_status_t st = sequence_render(seq, reset ? 0 : index, reset);
    if (st == OS64_IMAGE_OK && reset && seq->view.play_count) seq->pass++;
    return st;
}

os64_image_status_t os64_image_sequence_decode(const uint8_t *data, size_t len,
                                               os64_image_sequence_t **out)
{
    if (!out) return OS64_IMAGE_MALFORMED;
    *out = NULL;
    bool gif = data && len >= 6 && !os64_memcmp(data, "GIF8", 4) &&
               (data[4] == '7' || data[4] == '9') && data[5] == 'a';
    gif_scan_t scan;
    if (gif) {
        if (len > OS64_IMAGE_CAP_DEFAULT) return OS64_IMAGE_TOO_BIG;
        gif_reader_t r = {data, len, 0};
        os64_image_status_t st = parse(&r, &scan, NULL, 0, true);
        if (st != OS64_IMAGE_OK) return st;
        // Axes/pixels and frame count were checked by parse, so these terms
        // cannot overflow size_t. Include metadata and the copied input.
        size_t memory = sizeof(os64_image_sequence_t) + len +
            scan.count * sizeof(gif_frame_t) + sizeof(gif_dictionary_t) +
            (size_t)scan.width * scan.height * 4 + scan.max_raster + scan.max_restore * 4;
        if (memory > GIF_SEQUENCE_MEMORY) return OS64_IMAGE_LIMIT;
    }
    os64_image_sequence_t *seq = os64_malloc(sizeof(*seq));
    if (!seq) return OS64_IMAGE_NO_MEMORY;
    os64_memset(seq, 0, sizeof(*seq));
    os64_image_status_t status;
    if (!gif) {
        os64_image_t still;
        status = os64_image_decode(data, len, &still);
        if (status != OS64_IMAGE_OK) goto fail;
        seq->canvas = still.pixels;
        seq->view = (os64_image_frame_t){.width = still.width, .height = still.height,
            .pixels = still.pixels, .frame_count = 1, .play_count = 1};
    } else {
        if (!(seq->data = os64_malloc(len)) ||
            !(seq->frames = os64_malloc(scan.count * sizeof(gif_frame_t))) ||
            !(seq->canvas = os64_malloc((size_t)scan.width * scan.height * 4)) ||
            !(seq->indices = os64_malloc(scan.max_raster)) ||
            !(seq->dict = os64_malloc(sizeof(*seq->dict))) ||
            (scan.max_restore && !(seq->restore = os64_malloc(scan.max_restore * 4)))) {
            status = OS64_IMAGE_NO_MEMORY;
            goto fail;
        }
        os64_memcpy(seq->data, data, len);
        seq->view = (os64_image_frame_t){.width = scan.width, .height = scan.height,
            .pixels = seq->canvas, .frame_count = (uint32_t)scan.count, .play_count = scan.plays};
        gif_reader_t r = {seq->data, len, 0};
        status = parse(&r, &scan, seq->frames, scan.count, true);
        if (status != OS64_IMAGE_OK) goto fail;
        status = sequence_render(seq, 0, true);
        if (status != OS64_IMAGE_OK) goto fail;
    }
    seq->pass = 1;
    *out = seq;
    return OS64_IMAGE_OK;
fail:
    os64_image_sequence_free(seq);
    return status;
}

os64_image_status_t os64_image_sequence_load(const char *path, size_t cap,
                                             os64_image_sequence_t **out)
{
    if (!out) return OS64_IMAGE_MALFORMED;
    *out = NULL;
    uint8_t *data;
    size_t len;
    os64_slurp_status_t st = os64_slurp(path, cap ? cap : OS64_IMAGE_CAP_DEFAULT, &data, &len);
    switch (st) {
    case OS64_SLURP_NO_FILE: return OS64_IMAGE_NO_FILE;
    case OS64_SLURP_TOO_BIG: return OS64_IMAGE_TOO_BIG;
    case OS64_SLURP_IO_ERROR: return OS64_IMAGE_IO_ERROR;
    case OS64_SLURP_NO_MEMORY: return OS64_IMAGE_NO_MEMORY;
    case OS64_SLURP_OK: break;
    }
    os64_image_status_t result = os64_image_sequence_decode(data, len, out);
    os64_free(data);
    return result;
}
