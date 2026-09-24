#ifndef OS64_DECORATION_H
#define OS64_DECORATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Prepared assets, little-endian on os64's x86-64 ABI. This revision supplies
 * top-titlebar typography, controls and prepared opaque finishes. Unsupported
 * fields are refused. */
#define OS64_DECOR_MAGIC 0x31434544u
#define OS64_DECOR_VERSION 6u
#define OS64_DECOR_LEGACY_HEADER_BYTES 296u
#define OS64_DECOR_BYTES_MAX (8u * 1024u * 1024u)
#define OS64_DECOR_GLYPHS_MAX 768u
#define OS64_DECOR_PAIRS_MAX 65536u
#define OS64_DECOR_MASK_MAX 256u
#define OS64_DECOR_TITLE_MAX 128u
#define OS64_DECOR_EDGE_TOP 0u
#define OS64_DECOR_TILE_SIDE 32u
#define OS64_DECOR_TILE_BYTES (4u*OS64_DECOR_TILE_SIDE*OS64_DECOR_TILE_SIDE*4u)
/* Status appends a versioned content fingerprint to the generation line.
 * FNV-1a-64 plus byte length identifies prepared bytes for local collection
 * lookup; it is not an authentication digest. Zero bytes means no fingerprint
 * (built-in decoration or a legacy generation-only status response). */
#define OS64_DECOR_STATUS_MAX 96u
typedef struct {
    uint64_t generation, fingerprint;
    uint32_t bytes;
} os64_decor_status_t;
/* Hash exactly length readable bytes; callers validate prepared assets first. */
uint64_t os64_decor_fingerprint(const void *,size_t);
/* Writer requires STATUS_MAX bytes and excludes NUL; reader retains out on failure. */
size_t os64_decor_status_write(char *,const os64_decor_status_t *);
bool os64_decor_status_read(const char *,size_t,os64_decor_status_t *);

#define OS64_DECOR_SOLID 0u
#define OS64_DECOR_GRADIENT 1u
#define OS64_DECOR_GRAIN 2u
#define OS64_DECOR_STRIPES 3u
#define OS64_DECOR_STIPPLE 4u

/* Each command fits one syscall I/O chunk. BEGIN reserves a private bundle;
 * DATA requires its exact next offset; COMMIT returns acceptance on write.
 * Close discards unfinished staging. Generation is checked at COMMIT. */
#define OS64_DECOR_BEGIN 1u
#define OS64_DECOR_DATA 2u
#define OS64_DECOR_COMMIT 3u
#define OS64_DECOR_DATA_MAX 1024u
typedef struct {
    uint32_t command, total_bytes, offset, data_bytes;
    uint64_t expected_generation;
} os64_decor_command_t;
_Static_assert(sizeof(os64_decor_command_t)==24, "decoration command ABI");

#define OS64_DECOR_BUTTONS_MAX 8u
#define OS64_DECOR_SPACER 0u
#define OS64_DECOR_CLOSE 1u
#define OS64_DECOR_MINIMIZE 2u
#define OS64_DECOR_MAXIMIZE 3u
#define OS64_DECOR_PIN 4u
#define OS64_DECOR_SQUARE 0u
#define OS64_DECOR_ROUND 1u
#define OS64_DECOR_BARE 2u
/* Slots are ordered left-to-right within group 0 (leading) or 1 (trailing).
 * A spacer occupies one button width but has no paint or hit target. */
/* face: zero keeps the automatic title-derived housing. FFrrggbb uses an
 * opaque custom color; 01rrggbb suppresses the housing in every state and
 * retains the custom RGB for switching back. V4 bundles require zero here. */
#define OS64_DECOR_FACE_TRANSPARENT 0x01000000u
typedef struct { uint32_t action, group, shape, face; } os64_decor_button_t;

typedef struct { uint32_t active, inactive; } os64_decor_symbol_t;

typedef struct {
    uint32_t magic, version, bytes, edge;
    uint32_t glyph_count, pair_count, glyph_offset, pair_offset;
    uint32_t mask_offset, mask_bytes, line_height, baseline;
    uint32_t border, padding_x, padding_y, align;
    uint32_t active_face, inactive_face, active_text, inactive_text;
    uint32_t active_border, inactive_border, reserved[2];
    uint32_t button_count, button_size, button_gap, button_reserved;
    os64_decor_button_t buttons[OS64_DECOR_BUTTONS_MAX];
    /* Four 32x32 XRGB tiles: active/inactive title, active/inactive border.
     * Gradients stretch; other tiles repeat. Scale is 1, 2, 4 or 8. */
    uint32_t tile_offset, tile_bytes, finish, border_finish;
    uint32_t strength, scale, direction, seed, relief, match_border;
    /* Gradients span each color pair. Pattern strength (0..64) blends the
     * second color toward the first; Solid ignores the second color. */
    uint32_t active_face2, inactive_face2, active_border2, inactive_border2;
    /* V6: action-minus-one order (close/minimize/maximize/pin). Zero follows
     * title text; FFrrggbb overrides that state's symbol. Absent buttons keep
     * their choices so removal/reordering does not transfer another action's ink. */
    os64_decor_symbol_t symbols[4];
} os64_decor_header_t;

typedef struct {
    uint32_t scalar, offset, width, height;
    int32_t left, top, advance;
    uint32_t reserved;
} os64_decor_glyph_t;

/* key = left glyph ordinal << 16 | right glyph ordinal; sorted, unique.
 * Advance and adjustment are signed 26.6 pixels, matching the text backend. */
typedef struct { uint32_t key; int32_t adjustment; } os64_decor_pair_t;

typedef struct {
    const os64_decor_header_t *header;
    const os64_decor_glyph_t *glyphs;
    const os64_decor_pair_t *pairs;
    const uint8_t *masks;
    const uint32_t *tiles;
} os64_decor_view_t;

typedef struct { int32_t x, y, w, h; } os64_decor_rect_t;
typedef struct { int32_t left, top, right, bottom; } os64_decor_insets_t;
typedef struct {
    os64_decor_insets_t insets;
    os64_decor_rect_t title, text;
    int32_t baseline;
    os64_decor_rect_t buttons[OS64_DECOR_BUTTONS_MAX];
} os64_decor_layout_t;
typedef struct {
    uint32_t *pixels;
    uint32_t width, height, pitch;
} os64_decor_surface_t;

_Static_assert(sizeof(os64_decor_header_t) == 328, "decoration header ABI");
_Static_assert(offsetof(os64_decor_header_t,symbols)==OS64_DECOR_LEGACY_HEADER_BYTES,"legacy decoration prefix");
_Static_assert(sizeof(os64_decor_glyph_t) == 32, "decoration glyph ABI");
_Static_assert(sizeof(os64_decor_pair_t) == 8, "decoration pair ABI");

/* Validation makes a borrowed view of immutable, 4-byte-aligned storage.
 * Failure clears out. The caller owns storage and serializes its lifetime. */
bool os64_decor_validate(const void *bytes, size_t length, os64_decor_view_t *out);
/* Copy a validated header into a full current recipe, inheriting old symbols.
 * Output must be distinct from the borrowed header. Asset offsets stay intact. */
void os64_decor_header_copy(const os64_decor_header_t *,os64_decor_header_t *);
os64_decor_insets_t os64_decor_insets(const os64_decor_header_t *, bool titlebar);
bool os64_decor_layout(const os64_decor_header_t *, int32_t width, int32_t height,
    bool titlebar, os64_decor_layout_t *out);

/* Geometry and hits require a validated header; action zero means no hit. */
uint32_t os64_decor_min_width(const os64_decor_header_t *);
uint32_t os64_decor_hit(const os64_decor_header_t *, const os64_decor_layout_t *, int32_t x, int32_t y);
typedef struct { uint32_t hover, pressed, disabled; bool maximized; } os64_decor_state_t;

/* Capture keeps consumed edges after cancellation. The owner supplies the
 * current hit identity; geometry/VT changes cancel before stepping events. */
typedef struct { uint32_t window, action, buttons; bool armed; } os64_decor_capture_t;
#define OS64_DECOR_POINTER_MOVE 0u
#define OS64_DECOR_POINTER_DOWN 1u
#define OS64_DECOR_POINTER_UP 2u
void os64_decor_capture_cancel(os64_decor_capture_t *);
void os64_decor_capture_begin(os64_decor_capture_t *, uint32_t window, uint32_t action, uint32_t buttons);
uint32_t os64_decor_capture_step(os64_decor_capture_t *, uint32_t event, uint32_t button,
    uint32_t hit_window, uint32_t hit_action);

/* A validated view is required. Paint is allocation-free and clips to both
 * surface and damage. Clipping does not change frame-relative text placement.
 * legacy_title selects Latin-1 bytes; false selects the shared W1 profile. */
bool os64_decor_paint(const os64_decor_view_t *, os64_decor_surface_t *,
    os64_decor_rect_t frame, os64_decor_rect_t damage, bool titlebar, bool active,
    bool pinned, const char *title, size_t length, bool legacy_title, const os64_decor_state_t *state);

#endif
