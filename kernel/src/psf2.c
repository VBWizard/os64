// psf2.c — see psf2.h. Pure: stdint, the ABI charset table, nothing else.

#include "psf2.h"
#include "os64/charset.h"

#define PSF2_HEADER_BYTES 32u
#define PSF2_FLAG_TABLE   0x01u

const char *psf2_status_name(psf2_status_t status)
{
    static const char *const names[] = {
        "ok", "image shorter than a header", "image too large", "not a PSF2 font",
        "unknown PSF2 version", "bad header size", "cell size outside the limits",
        "glyph size disagrees with the cell", "glyph count outside the limits",
        "glyphs truncated", "malformed Unicode table", "printable ASCII has no glyph",
    };
    return (unsigned)status < sizeof(names) / sizeof(names[0]) ? names[status] : "unknown";
}

// Byte-wise, because the image is wherever the writer put it: no alignment
// is promised, and a cast to uint32_t* would be a trap on the host's UBSan
// and a lie everywhere else.
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static psf2_status_t refuse(psf2_status_t status, uint32_t value, uint32_t *offender)
{
    if (offender != NULL)
        *offender = value;
    return status;
}

psf2_status_t psf2_parse(const uint8_t *image, size_t bytes,
                         psf2_face_t *out, uint32_t *offender)
{
    if (image == NULL || out == NULL || bytes < PSF2_HEADER_BYTES)
        return refuse(PSF2_TOO_SHORT, (uint32_t)bytes, offender);
    if (bytes > PSF2_IMAGE_MAX)
        return refuse(PSF2_TOO_BIG, (uint32_t)(bytes > UINT32_MAX ? UINT32_MAX : bytes), offender);
    if (image[0] != 0x72 || image[1] != 0xB5 || image[2] != 0x4A || image[3] != 0x86)
        return refuse(PSF2_BAD_MAGIC, le32(image), offender);

    uint32_t version     = le32(image + 4);
    uint32_t header_size = le32(image + 8);
    uint32_t flags       = le32(image + 12);
    uint32_t nglyphs     = le32(image + 16);
    uint32_t glyph_bytes = le32(image + 20);
    uint32_t height      = le32(image + 24);
    uint32_t width       = le32(image + 28);

    if (version != 0)
        return refuse(PSF2_BAD_VERSION, version, offender);
    if (header_size < PSF2_HEADER_BYTES || header_size > bytes)
        return refuse(PSF2_BAD_HEADER_SIZE, header_size, offender);
    if (width < PSF2_CELL_W_MIN || width > PSF2_CELL_W_MAX)
        return refuse(PSF2_BAD_CELL, width, offender);
    if (height < PSF2_CELL_H_MIN || height > PSF2_CELL_H_MAX)
        return refuse(PSF2_BAD_CELL, height, offender);

    // Every quantity below is bounded by the checks above it, so none of the
    // products can wrap: row_bytes <= 8, glyph_bytes <= 1024, nglyphs <= 65535.
    uint32_t row_bytes = (width + 7) / 8;
    if (glyph_bytes != height * row_bytes)
        return refuse(PSF2_BAD_GLYPH_BYTES, glyph_bytes, offender);
    if (nglyphs < PSF2_GLYPHS_MIN || nglyphs > PSF2_GLYPHS_MAX)
        return refuse(PSF2_BAD_GLYPH_COUNT, nglyphs, offender);

    uint64_t glyph_end = (uint64_t)header_size + (uint64_t)nglyphs * glyph_bytes;
    if (glyph_end > bytes)
        return refuse(PSF2_TRUNCATED, nglyphs, offender);

    *out = (psf2_face_t){
        .glyphs = image + header_size,
        .nglyphs = nglyphs,
        .width = width,
        .height = height,
        .row_bytes = row_bytes,
        .glyph_bytes = glyph_bytes,
        .table = NULL,
        .table_bytes = 0,
    };
    // A flagged table of zero length is still a table: build_charmap will
    // find it names nothing and refuse it there, by the right name.
    if (flags & PSF2_FLAG_TABLE) {
        out->table = image + glyph_end;
        out->table_bytes = bytes - (size_t)glyph_end;
    }
    return PSF2_OK;
}

// One code point out of the table's UTF-8. Returns the bytes consumed, or 0
// for anything that is not a well-formed sequence that fits before `end` —
// overlong forms and surrogates included, because a table that spells U+0041
// three ways is a table trying to claim 'A' for a glyph the author hid.
static size_t utf8_one(const uint8_t *p, const uint8_t *end, uint32_t *cp)
{
    if (p >= end)
        return 0;
    uint8_t b = p[0];
    size_t need;
    uint32_t value, floor;
    if (b < 0x80)      { *cp = b; return 1; }
    else if ((b & 0xE0) == 0xC0) { need = 1; value = b & 0x1F; floor = 0x80; }
    else if ((b & 0xF0) == 0xE0) { need = 2; value = b & 0x0F; floor = 0x800; }
    else if ((b & 0xF8) == 0xF0) { need = 3; value = b & 0x07; floor = 0x10000; }
    else return 0;
    if ((size_t)(end - p) <= need)
        return 0;
    for (size_t i = 1; i <= need; i++) {
        if ((p[i] & 0xC0) != 0x80)
            return 0;
        value = (value << 6) | (p[i] & 0x3F);
    }
    if (value < floor || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF))
        return 0;
    *cp = value;
    return need + 1;
}

static void claim(psf2_charmap_t *map, uint32_t cp, uint16_t glyph)
{
    if (cp < 256 && map->glyph[OS64_CHARSET_LATIN1][cp] == PSF2_MAP_NONE)
        map->glyph[OS64_CHARSET_LATIN1][cp] = glyph;
    // CP437 is not a range of Unicode, so the question is asked backwards:
    // which bytes mean this code point? Usually one, and 256 compares per
    // table entry is nothing beside the disk read that fetched the font.
    for (uint32_t b = 0; b < 256; b++)
        if (os64_cp437_codepoint((uint8_t)b) == cp &&
            map->glyph[OS64_CHARSET_CP437][b] == PSF2_MAP_NONE)
            map->glyph[OS64_CHARSET_CP437][b] = glyph;
}

psf2_status_t psf2_build_charmap(const psf2_face_t *face, psf2_charmap_t *out,
                                 uint32_t *offender)
{
    if (face == NULL || out == NULL)
        return refuse(PSF2_BAD_TABLE, 0, offender);

    psf2_charmap_t map;
    for (uint32_t s = 0; s < 2; s++)
        for (uint32_t b = 0; b < 256; b++)
            map.glyph[s][b] = PSF2_MAP_NONE;
    map.from_table = face->table != NULL;
    map.unmapped[0] = map.unmapped[1] = 0;

    if (face->table == NULL) {
        for (uint32_t s = 0; s < 2; s++)
            for (uint32_t b = 0; b < 256 && b < face->nglyphs; b++)
                map.glyph[s][b] = (uint16_t)b;
    } else {
        const uint8_t *p = face->table, *end = face->table + face->table_bytes;
        uint32_t glyph = 0;
        bool in_sequences = false;
        while (p < end && glyph < face->nglyphs) {
            if (*p == 0xFF) { glyph++; in_sequences = false; p++; continue; }
            if (*p == 0xFE) { in_sequences = true; p++; continue; }
            uint32_t cp;
            size_t used = utf8_one(p, end, &cp);
            if (used == 0)
                return refuse(PSF2_BAD_TABLE, glyph, offender);
            if (!in_sequences)
                claim(&map, cp, (uint16_t)glyph);
            p += used;
        }
        // Every glyph has an entry, even an empty one, so a table that runs
        // out early was cut short — and the glyphs it never reached would
        // silently draw nothing.
        if (glyph < face->nglyphs)
            return refuse(PSF2_BAD_TABLE, glyph, offender);
    }

    // What a person would see missing. DEL is not a character in either
    // set, and Latin-1's 0x80..0x9F are the C1 controls, which no font draws.
    for (uint32_t s = 0; s < 2; s++)
        for (uint32_t b = 0x20; b < 256; b++) {
            bool control = b == 0x7F ||
                           (s == OS64_CHARSET_LATIN1 && b >= 0x80 && b < 0xA0);
            if (!control && map.glyph[s][b] == PSF2_MAP_NONE)
                map.unmapped[s]++;
        }
    for (uint32_t b = 0x20; b < 0x7F; b++)
        if (map.glyph[OS64_CHARSET_LATIN1][b] == PSF2_MAP_NONE)
            return refuse(PSF2_NO_ASCII, b, offender);

    *out = map;
    return PSF2_OK;
}

bool psf2_synth_block(uint32_t codepoint, uint32_t width, uint32_t height,
                      uint8_t *out)
{
    if (out == NULL || width == 0 || height == 0)
        return false;
    switch (codepoint) {
    case 0x2580: case 0x2584: case 0x2588: case 0x258C: case 0x2590:
    case 0x2591: case 0x2592: case 0x2593:
        break;
    default:
        return false;
    }

    uint32_t row_bytes = (width + 7) / 8;
    for (uint32_t i = 0; i < height * row_bytes; i++)
        out[i] = 0;

    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            bool lit;
            switch (codepoint) {
            case 0x2580: lit = y < height / 2; break;             // upper half
            case 0x2584: lit = y >= height / 2; break;            // lower half
            case 0x258C: lit = x < width / 2; break;              // left half
            case 0x2590: lit = x >= width / 2; break;             // right half
            case 0x2588: lit = true; break;                       // full block
            // The shades are the VGA ROM's dither: one pixel in four, a
            // checkerboard, and the first one's negative — the same family
            // os64/charset.h's 8x16 bitmaps belong to (0x88/0x22, inverted).
            case 0x2591: lit = (x + 2 * (y & 1)) % 4 == 0; break;
            case 0x2592: lit = ((x + y) & 1) == 0; break;
            default:     lit = (x + 2 * (y & 1)) % 4 != 0; break; // U+2593
            }
            if (lit)
                out[y * row_bytes + (x >> 3)] |= (uint8_t)(0x80u >> (x & 7));
        }
    }
    return true;
}
