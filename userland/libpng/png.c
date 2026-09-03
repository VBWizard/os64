// png.c — bounded decoding for the PNG datastream and its zlib payload.
//
// The input is untrusted. The first pass validates chunk framing and ordering
// before allocating from dimensions in IHDR. Bad critical CRCs are fatal; a
// bad ancillary CRC discards that chunk without trusting its contents. The
// second pass feeds IDAT directly to libgzip's raw inflater: compressed data is
// never copied into a second whole-file buffer, and decoded data is held one
// scanline at a time.

#include "png/png.h"

#include "gzip/inflate.h"
#include "os64/crc32.h"
#include "os64/mem.h"
#include "os64/str.h"

#include <stdbool.h>

#define PNG_SIGNATURE_SIZE 8u
#define PNG_CHUNK_OVERHEAD 12u
#define PNG_ADLER_MOD 65521u

#define PNG_CHUNK(a, b, c, d) \
    (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | \
     ((uint32_t)(c) << 8) | (uint32_t)(d))

#define PNG_IHDR PNG_CHUNK('I', 'H', 'D', 'R')
#define PNG_PLTE PNG_CHUNK('P', 'L', 'T', 'E')
#define PNG_IDAT PNG_CHUNK('I', 'D', 'A', 'T')
#define PNG_IEND PNG_CHUNK('I', 'E', 'N', 'D')
#define PNG_tRNS PNG_CHUNK('t', 'R', 'N', 'S')

typedef struct {
    uint32_t width;
    uint32_t height;
    size_t row_bytes;
    uint64_t filtered_size;
    uint8_t bit_depth;
    uint8_t color_type;
    uint8_t channels;
    uint8_t filter_bpp;
    uint16_t palette_count;
    uint32_t palette[256];
    bool has_transparent_gray;
    bool has_transparent_rgb;
    uint16_t transparent_gray;
    uint16_t transparent_red;
    uint16_t transparent_green;
    uint16_t transparent_blue;
} png_info_t;

typedef enum {
    CHUNKS_BEFORE_IDAT,
    CHUNKS_IN_IDAT,
    CHUNKS_AFTER_IDAT
} png_chunk_phase_t;

typedef struct {
    uint32_t first;
    uint32_t second;
} png_adler_t;

static const uint8_t kPngSignature[PNG_SIGNATURE_SIZE] = {
    137, 80, 78, 71, 13, 10, 26, 10
};

static uint16_t read_be16(const uint8_t *bytes)
{
    return (uint16_t)((uint16_t)bytes[0] << 8 | bytes[1]);
}

static uint32_t read_be32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 |
           (uint32_t)bytes[2] << 8 | (uint32_t)bytes[3];
}

static bool bytes_equal(const uint8_t *left, const uint8_t *right,
                        size_t length)
{
    for (size_t i = 0; i < length; i++)
        if (left[i] != right[i])
            return false;
    return true;
}

static bool chunk_type_letters_valid(const uint8_t *name)
{
    for (size_t i = 0; i < 4; i++) {
        uint8_t c = name[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')))
            return false;
    }
    return true;
}

static bool depth_valid(uint8_t color_type, uint8_t depth)
{
    switch (color_type) {
        case 0: return depth == 1 || depth == 2 || depth == 4 ||
                       depth == 8 || depth == 16;
        case 2: return depth == 8 || depth == 16;
        case 3: return depth == 1 || depth == 2 || depth == 4 || depth == 8;
        case 4: return depth == 8 || depth == 16;
        case 6: return depth == 8 || depth == 16;
    }
    return false;
}

static uint8_t color_channels(uint8_t color_type)
{
    switch (color_type) {
        case 0: return 1;
        case 2: return 3;
        case 3: return 1;
        case 4: return 2;
        case 6: return 4;
    }
    return 0;
}

static os64_png_status_t parse_ihdr(const uint8_t *chunk, uint32_t length,
                                    png_info_t *info, bool *unsupported)
{
    if (length != 13)
        return OS64_PNG_MALFORMED;

    info->width = read_be32(chunk);
    info->height = read_be32(chunk + 4);
    info->bit_depth = chunk[8];
    info->color_type = chunk[9];
    uint8_t compression = chunk[10];
    uint8_t filter = chunk[11];
    uint8_t interlace = chunk[12];

    if (info->width == 0 || info->height == 0)
        return OS64_PNG_MALFORMED;
    if (info->width > OS64_PNG_DIM_MAX || info->height > OS64_PNG_DIM_MAX)
        return OS64_PNG_LIMIT;
    if (!depth_valid(info->color_type, info->bit_depth))
        return OS64_PNG_MALFORMED;
    if (compression != 0 || filter != 0 || interlace > 1)
        return OS64_PNG_MALFORMED;
    if (interlace == 1)
        *unsupported = true;

    info->channels = color_channels(info->color_type);
    uint64_t row_bits = (uint64_t)info->width * info->channels *
                        info->bit_depth;
    info->row_bytes = (size_t)((row_bits + 7u) / 8u);
    info->filter_bpp = (uint8_t)((info->channels * info->bit_depth + 7u) / 8u);
    info->filtered_size = (uint64_t)info->height *
                          ((uint64_t)info->row_bytes + 1u);
    return OS64_PNG_OK;
}

static os64_png_status_t parse_palette(const uint8_t *chunk, uint32_t length,
                                       png_info_t *info)
{
    if (info->color_type == 0 || info->color_type == 4 || length == 0 ||
        length > 256u * 3u || length % 3u != 0)
        return OS64_PNG_MALFORMED;

    uint16_t count = (uint16_t)(length / 3u);
    if (info->color_type == 3 && count > (1u << info->bit_depth))
        return OS64_PNG_MALFORMED;

    info->palette_count = count;
    for (uint16_t i = 0; i < count; i++) {
        const uint8_t *entry = chunk + (size_t)i * 3u;
        info->palette[i] = 0xff000000u | (uint32_t)entry[0] << 16 |
                           (uint32_t)entry[1] << 8 | entry[2];
    }
    return OS64_PNG_OK;
}

static os64_png_status_t parse_transparency(const uint8_t *chunk,
                                            uint32_t length,
                                            png_info_t *info)
{
    if (info->color_type == 0) {
        if (length != 2)
            return OS64_PNG_MALFORMED;
        info->transparent_gray = read_be16(chunk);
        uint32_t max = (1u << info->bit_depth) - 1u;
        if (info->bit_depth == 16)
            max = 65535u;
        if (info->transparent_gray > max)
            return OS64_PNG_MALFORMED;
        info->has_transparent_gray = true;
        return OS64_PNG_OK;
    }
    if (info->color_type == 2) {
        if (length != 6)
            return OS64_PNG_MALFORMED;
        info->transparent_red = read_be16(chunk);
        info->transparent_green = read_be16(chunk + 2);
        info->transparent_blue = read_be16(chunk + 4);
        uint32_t max = info->bit_depth == 16 ? 65535u : 255u;
        if (info->transparent_red > max || info->transparent_green > max ||
            info->transparent_blue > max)
            return OS64_PNG_MALFORMED;
        info->has_transparent_rgb = true;
        return OS64_PNG_OK;
    }
    if (info->color_type == 3) {
        if (info->palette_count == 0 || length == 0 ||
            length > info->palette_count)
            return OS64_PNG_MALFORMED;
        for (uint32_t i = 0; i < length; i++)
            info->palette[i] = (info->palette[i] & 0x00ffffffu) |
                               (uint32_t)chunk[i] << 24;
        return OS64_PNG_OK;
    }
    return OS64_PNG_MALFORMED; // tRNS is forbidden for alpha-bearing types.
}

static os64_png_status_t parse_chunks(const uint8_t *data, size_t length,
                                      png_info_t *info)
{
    if (length < PNG_SIGNATURE_SIZE ||
        !bytes_equal(data, kPngSignature, PNG_SIGNATURE_SIZE))
        return OS64_PNG_NOT_PNG;

    os64_memset(info, 0, sizeof(*info));
    size_t offset = PNG_SIGNATURE_SIZE;
    png_chunk_phase_t phase = CHUNKS_BEFORE_IDAT;
    bool seen_ihdr = false;
    bool seen_plte = false;
    bool seen_trns = false;
    bool seen_idat = false;
    bool unsupported = false;

    while (offset < length) {
        if (length - offset < PNG_CHUNK_OVERHEAD)
            return OS64_PNG_MALFORMED;
        uint32_t chunk_length = read_be32(data + offset);
        const uint8_t *type_bytes = data + offset + 4u;
        if (chunk_length > 0x7fffffffu ||
            !chunk_type_letters_valid(type_bytes) ||
            (size_t)chunk_length > length - offset - PNG_CHUNK_OVERHEAD)
            return OS64_PNG_MALFORMED;

        uint32_t type = read_be32(type_bytes);
        const uint8_t *chunk = data + offset + 8u;
        uint32_t wanted_crc = read_be32(chunk + chunk_length);
        uint32_t crc = os64_crc32_begin();
        crc = os64_crc32_update(crc, type_bytes, 4);
        crc = os64_crc32_update(crc, chunk, chunk_length);
        bool crc_matches = os64_crc32_end(crc) == wanted_crc;
        bool ancillary = (type_bytes[0] & 0x20u) != 0;
        if (!crc_matches && !ancillary)
            return OS64_PNG_CHECKSUM;

        if (!seen_ihdr && type != PNG_IHDR)
            return OS64_PNG_MALFORMED;

        os64_png_status_t status = OS64_PNG_OK;
        if (!crc_matches) {
            // Recovery is safe only by treating the entire ancillary chunk as
            // unknown. Its position still ends a consecutive IDAT sequence.
            if (phase == CHUNKS_IN_IDAT)
                phase = CHUNKS_AFTER_IDAT;
        } else if (type == PNG_IHDR) {
            if (seen_ihdr || offset != PNG_SIGNATURE_SIZE)
                return OS64_PNG_MALFORMED;
            seen_ihdr = true;
            status = parse_ihdr(chunk, chunk_length, info, &unsupported);
        } else if (type == PNG_PLTE) {
            if (seen_plte || phase != CHUNKS_BEFORE_IDAT)
                return OS64_PNG_MALFORMED;
            seen_plte = true;
            status = parse_palette(chunk, chunk_length, info);
        } else if (type == PNG_tRNS) {
            if (seen_trns || phase != CHUNKS_BEFORE_IDAT)
                return OS64_PNG_MALFORMED;
            seen_trns = true;
            status = parse_transparency(chunk, chunk_length, info);
        } else if (type == PNG_IDAT) {
            if (phase == CHUNKS_AFTER_IDAT ||
                (info->color_type == 3 && !seen_plte))
                return OS64_PNG_MALFORMED;
            phase = CHUNKS_IN_IDAT;
            seen_idat = true;
        } else if (type == PNG_IEND) {
            if (!seen_idat || chunk_length != 0)
                return OS64_PNG_MALFORMED;
            offset += PNG_CHUNK_OVERHEAD;
            if (offset != length)
                return OS64_PNG_MALFORMED;
            return unsupported ? OS64_PNG_UNSUPPORTED : OS64_PNG_OK;
        } else {
            if ((type_bytes[0] & 0x20u) == 0)
                unsupported = true;
            if (phase == CHUNKS_IN_IDAT)
                phase = CHUNKS_AFTER_IDAT;
        }
        if (status != OS64_PNG_OK)
            return status;

        offset += (size_t)chunk_length + PNG_CHUNK_OVERHEAD;
    }
    return OS64_PNG_MALFORMED; // no IEND
}

static uint8_t paeth_predictor(uint8_t left, uint8_t up, uint8_t upper_left)
{
    int32_t p = (int32_t)left + up - upper_left;
    int32_t pa = p - left;
    int32_t pb = p - up;
    int32_t pc = p - upper_left;
    if (pa < 0) pa = -pa;
    if (pb < 0) pb = -pb;
    if (pc < 0) pc = -pc;
    if (pa <= pb && pa <= pc) return left;
    if (pb <= pc) return up;
    return upper_left;
}

static bool reverse_filter(uint8_t *row, const uint8_t *previous,
                           size_t length, uint8_t bpp, uint8_t filter)
{
    for (size_t i = 0; i < length; i++) {
        uint8_t left = i >= bpp ? row[i - bpp] : 0;
        uint8_t up = previous == NULL ? 0 : previous[i];
        uint8_t upper_left = previous != NULL && i >= bpp
                           ? previous[i - bpp] : 0;
        switch (filter) {
            case 0: break;
            case 1: row[i] = (uint8_t)(row[i] + left); break;
            case 2: row[i] = (uint8_t)(row[i] + up); break;
            case 3:
                row[i] = (uint8_t)(row[i] +
                                   ((uint32_t)left + up) / 2u);
                break;
            case 4:
                row[i] = (uint8_t)(row[i] +
                                   paeth_predictor(left, up, upper_left));
                break;
            default: return false;
        }
    }
    return true;
}

static uint16_t row_sample(const uint8_t *row, uint64_t sample,
                           uint8_t depth)
{
    if (depth == 8)
        return row[sample];
    if (depth == 16)
        return read_be16(row + sample * 2u);
    uint64_t bit = sample * depth;
    uint8_t shift = (uint8_t)(8u - depth - (bit & 7u));
    return (uint16_t)((row[bit >> 3] >> shift) & ((1u << depth) - 1u));
}

static uint8_t sample_to_byte(uint16_t sample, uint8_t depth)
{
    if (depth == 8)
        return (uint8_t)sample;
    if (depth == 16)
        return (uint8_t)(sample >> 8);
    uint32_t max = (1u << depth) - 1u;
    return (uint8_t)(((uint32_t)sample * 255u + max / 2u) / max);
}

static bool convert_row(const png_info_t *info, const uint8_t *row,
                        uint32_t *pixels)
{
    uint64_t sample = 0;
    for (uint32_t x = 0; x < info->width; x++) {
        uint16_t red, green, blue, alpha = 255;
        if (info->color_type == 0) {
            uint16_t gray = row_sample(row, sample++, info->bit_depth);
            red = green = blue = sample_to_byte(gray, info->bit_depth);
            if (info->has_transparent_gray && gray == info->transparent_gray)
                alpha = 0;
        } else if (info->color_type == 2) {
            uint16_t r = row_sample(row, sample++, info->bit_depth);
            uint16_t g = row_sample(row, sample++, info->bit_depth);
            uint16_t b = row_sample(row, sample++, info->bit_depth);
            red = sample_to_byte(r, info->bit_depth);
            green = sample_to_byte(g, info->bit_depth);
            blue = sample_to_byte(b, info->bit_depth);
            if (info->has_transparent_rgb && r == info->transparent_red &&
                g == info->transparent_green && b == info->transparent_blue)
                alpha = 0;
        } else if (info->color_type == 3) {
            uint16_t index = row_sample(row, sample++, info->bit_depth);
            if (index >= info->palette_count)
                return false;
            pixels[x] = info->palette[index];
            continue;
        } else if (info->color_type == 4) {
            uint16_t gray = row_sample(row, sample++, info->bit_depth);
            uint16_t a = row_sample(row, sample++, info->bit_depth);
            red = green = blue = sample_to_byte(gray, info->bit_depth);
            alpha = sample_to_byte(a, info->bit_depth);
        } else {
            uint16_t r = row_sample(row, sample++, info->bit_depth);
            uint16_t g = row_sample(row, sample++, info->bit_depth);
            uint16_t b = row_sample(row, sample++, info->bit_depth);
            uint16_t a = row_sample(row, sample++, info->bit_depth);
            red = sample_to_byte(r, info->bit_depth);
            green = sample_to_byte(g, info->bit_depth);
            blue = sample_to_byte(b, info->bit_depth);
            alpha = sample_to_byte(a, info->bit_depth);
        }
        pixels[x] = (uint32_t)alpha << 24 | (uint32_t)red << 16 |
                    (uint32_t)green << 8 | blue;
    }
    return true;
}

static void adler_update(png_adler_t *adler, const uint8_t *bytes,
                         size_t length)
{
    for (size_t i = 0; i < length; i++) {
        adler->first += bytes[i];
        if (adler->first >= PNG_ADLER_MOD)
            adler->first -= PNG_ADLER_MOD;
        adler->second += adler->first;
        if (adler->second >= PNG_ADLER_MOD)
            adler->second -= PNG_ADLER_MOD;
    }
}

static os64_png_status_t map_inflate_status(os64_inflate_status_t status)
{
    switch (status) {
        case OS64_INFLATE_TRUNCATED: return OS64_PNG_MALFORMED;
        case OS64_INFLATE_MALFORMED: return OS64_PNG_MALFORMED;
        case OS64_INFLATE_LIMIT:
            // This cap is IHDR's exact filtered raster size. Exceeding it means
            // the stream contradicts the raster, not that a resource cap fired.
            return OS64_PNG_MALFORMED;
        case OS64_INFLATE_BAD_ARGUMENT: return OS64_PNG_BAD_ARGUMENT;
        case OS64_INFLATE_NEED_INPUT:
        case OS64_INFLATE_NEED_OUTPUT:
        case OS64_INFLATE_DONE:
            break;
    }
    return OS64_PNG_MALFORMED;
}

static os64_png_status_t finish_row(const png_info_t *info, uint8_t **current,
                                    uint8_t **previous, uint32_t row_index,
                                    uint32_t *pixels)
{
    uint8_t *filtered = *current;
    uint8_t *prior = row_index == 0 ? NULL : *previous + 1u;
    if (!reverse_filter(filtered + 1u, prior, info->row_bytes,
                        info->filter_bpp, filtered[0]) ||
        !convert_row(info, filtered + 1u,
                     pixels + (size_t)row_index * info->width))
        return OS64_PNG_MALFORMED;
    uint8_t *swap = *previous;
    *previous = *current;
    *current = swap;
    return OS64_PNG_OK;
}

static os64_png_status_t decode_idat(const uint8_t *data, size_t length,
                                     const png_info_t *info, uint32_t *pixels,
                                     uint8_t *rows,
                                     os64_inflate_t *inflate)
{
    size_t row_size = info->row_bytes + 1u;
    uint8_t *previous = rows;
    uint8_t *current = rows + row_size;
    size_t row_have = 0;
    uint32_t row_index = 0;
    uint8_t zlib_header[2];
    uint8_t header_have = 0;
    uint8_t trailer[4];
    uint8_t trailer_have = 0;
    bool deflate_done = false;
    png_adler_t adler = {1, 0};

    size_t offset = PNG_SIGNATURE_SIZE;
    while (offset < length) {
        uint32_t chunk_length = read_be32(data + offset);
        uint32_t type = read_be32(data + offset + 4u);
        const uint8_t *input = data + offset + 8u;
        size_t input_length = chunk_length;

        if (type == PNG_IDAT) {
            while (input_length != 0) {
                if (header_have < 2) {
                    zlib_header[header_have++] = *input++;
                    input_length--;
                    if (header_have == 2) {
                        uint16_t header = (uint16_t)((uint16_t)zlib_header[0]
                                                     << 8 | zlib_header[1]);
                        if ((zlib_header[0] & 15u) != 8 ||
                            (zlib_header[0] >> 4) > 7 || header % 31u != 0)
                            return OS64_PNG_MALFORMED;
                        // PNG forbids zlib preset dictionaries. This is a
                        // malformed PNG, not a valid variant awaiting support.
                        if ((zlib_header[1] & 0x20u) != 0)
                            return OS64_PNG_MALFORMED;
                    }
                    continue;
                }
                if (deflate_done) {
                    if (trailer_have == 4)
                        return OS64_PNG_MALFORMED;
                    trailer[trailer_have++] = *input++;
                    input_length--;
                    continue;
                }

                size_t output_length = row_index < info->height
                                     ? row_size - row_have : 0;
                uint8_t *output = current + row_have;
                const uint8_t *produced_at = output;
                size_t offered_output = output_length;
                os64_inflate_status_t ist = os64_inflate_process(
                    inflate, &input, &input_length, &output, &output_length,
                    false);
                size_t produced = offered_output - output_length;
                adler_update(&adler, produced_at, produced);
                row_have += produced;

                if (row_have == row_size) {
                    os64_png_status_t status = finish_row(
                        info, &current, &previous, row_index, pixels);
                    if (status != OS64_PNG_OK)
                        return status;
                    row_index++;
                    row_have = 0;
                }

                if (ist == OS64_INFLATE_DONE) {
                    if (row_index != info->height || row_have != 0 ||
                        os64_inflate_output_size(inflate) != info->filtered_size)
                        return OS64_PNG_MALFORMED;
                    deflate_done = true;
                } else if (ist == OS64_INFLATE_NEED_INPUT) {
                    if (input_length != 0)
                        return OS64_PNG_MALFORMED;
                } else if (ist == OS64_INFLATE_NEED_OUTPUT) {
                    // A complete row is consumed above. With rows remaining,
                    // the next pass offers a fresh row; with none remaining,
                    // the inflater's exact output limit distinguishes the
                    // end marker from one forbidden extra decoded byte.
                    if (row_index < info->height && row_have != 0)
                        return OS64_PNG_MALFORMED;
                } else {
                    return map_inflate_status(ist);
                }
            }
        }

        if (type == PNG_IEND)
            break;
        offset += (size_t)chunk_length + PNG_CHUNK_OVERHEAD;
    }

    if (header_have != 2 || !deflate_done || trailer_have != 4)
        return OS64_PNG_MALFORMED;
    uint32_t actual_adler = adler.second << 16 | adler.first;
    if (read_be32(trailer) != actual_adler)
        return OS64_PNG_CHECKSUM;
    return OS64_PNG_OK;
}

os64_png_status_t os64_png_decode(const uint8_t *data, size_t length,
                                  uint64_t pixel_cap,
                                  os64_png_image_t *out)
{
    if (out == NULL)
        return OS64_PNG_BAD_ARGUMENT;
    out->width = 0;
    out->height = 0;
    out->pixels = NULL;
    if (data == NULL)
        return OS64_PNG_BAD_ARGUMENT;

    png_info_t info;
    os64_png_status_t status = parse_chunks(data, length, &info);
    if (status != OS64_PNG_OK)
        return status;

    if (pixel_cap == 0)
        pixel_cap = OS64_PNG_PIXEL_CAP_DEFAULT;
    uint64_t pixel_count = (uint64_t)info.width * info.height;
    if (pixel_count > pixel_cap)
        return OS64_PNG_LIMIT;

    uint32_t *pixels = os64_malloc((size_t)pixel_count * sizeof(*pixels));
    size_t row_size = info.row_bytes + 1u;
    uint8_t *rows = os64_malloc(row_size * 2u);
    os64_inflate_t *inflate = os64_inflate_create(info.filtered_size);
    if (pixels == NULL || rows == NULL || inflate == NULL) {
        os64_free(pixels);
        os64_free(rows);
        os64_inflate_destroy(inflate);
        return OS64_PNG_NO_MEMORY;
    }

    status = decode_idat(data, length, &info, pixels, rows, inflate);
    os64_inflate_destroy(inflate);
    os64_free(rows);
    if (status != OS64_PNG_OK) {
        os64_free(pixels);
        return status;
    }

    out->width = info.width;
    out->height = info.height;
    out->pixels = pixels;
    return OS64_PNG_OK;
}

void os64_png_free(os64_png_image_t *image)
{
    if (image == NULL)
        return;
    os64_free(image->pixels);
    image->width = 0;
    image->height = 0;
    image->pixels = NULL;
}

const char *os64_png_status_name(os64_png_status_t status)
{
    switch (status) {
        case OS64_PNG_OK: return "ok";
        case OS64_PNG_NOT_PNG: return "not a PNG";
        case OS64_PNG_MALFORMED: return "malformed PNG";
        case OS64_PNG_UNSUPPORTED: return "unsupported PNG variant";
        case OS64_PNG_CHECKSUM: return "PNG checksum mismatch";
        case OS64_PNG_NO_MEMORY: return "out of memory";
        case OS64_PNG_LIMIT: return "decoded image exceeds limit";
        case OS64_PNG_BAD_ARGUMENT: return "bad argument";
    }
    return "unknown";
}
