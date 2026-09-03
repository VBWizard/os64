#ifndef OS64_IMAGE_H
#define OS64_IMAGE_H

// os64/image.h — libimage: FILE BYTES IN, PIXELS OUT. Nothing else.
//
// WHERE THIS LIVES AND WHY. Decoding an image is parsing an untrusted file,
// and that is the last work that belongs behind the ring boundary. Chris's
// ruling, 2026-08-25: "graphics decoders don't belong in the kernel".
//
// THE MIGRATION IS FINISHED (2026-08-25, the desktop-shell slice). os64's
// only decoder used to be a PPM parser inside `kernel/src/gui/desktop.c` —
// ring-0 code reading a file any user could write — and for one slice this
// library stood beside it, with gview(1) as its only user (Codex #30 rd1
// made this header say so rather than claim the move was done). That kernel
// file is DELETED now: /bin/desktop, the ring-3 shell, paints the wallpaper
// through this library, and nothing in ring 0 parses an image any more.
//
// THE LAYERING, which is deliberate and worth keeping: a decoder produces
// PIXELS. It knows nothing about windows, surfaces, or the compositor —
// there is not one GUI call in image.c. Drawing them is libdraw's job
// (os64_draw_blit), and libdraw in turn knows nothing about file formats.
// Either half can be tested without the other, and neither grows a
// dependency on the thing it feeds.
//
// THE OUTPUT FORMAT is the one the rest of the GUI already speaks:
// 0xAARRGGBB, one uint32_t per pixel, rows tightly packed (pitch == width).
// A surface's pitch is NOT its width (see os64_gui_surface_t), so blitting
// takes the source pitch as an argument rather than assuming either.
//
// FORMATS ARE RECOGNIZED BY MAGIC BYTES, NEVER BY EXTENSION. A file named
// .bmp holding a PPM decodes correctly; a file named .txt holding a BMP
// decodes correctly; and a truncated file is refused rather than guessed at.
// The extension is a hint to humans and has never been evidence.
//
//   P6  — binary PPM, 8 bits per sample. The format the wallpaper already
//         used, kept so existing images keep working.
//   BM  — Windows BMP, uncompressed (BI_RGB), 24 or 32 bits per pixel,
//         bottom-up or top-down.
//
// PNG IS DELIBERATELY ABSENT FROM THIS FOUNDATIONAL LIBRARY. The browser gave
// it the interop reason anticipated here, and libpng now owns its zlib
// framing, filters and color conversion on top of libgzip's raw DEFLATE door.
// gview links that codec selectively; cat and every other libos64 consumer do
// not pay for image parsing. LIBIMAGE.md records the target façade and the
// transitional home of this BMP/PPM code. OPAQUE BMP, to be exact (Codex #30
// rd8): this decoder forces every output alpha to 0xff, BI_BITFIELDS — the
// only BMP variant that can carry a real alpha mask — is on the UNSUPPORTED
// list, and os64_draw_blit copies without blending. The day artwork needs
// transparency, that is a slice of its own (alpha decode AND a blending
// blit), not a flag to flip here. Same test the ABI philosophy uses: no
// format compliance without an interop reason. GIF is
// not planned at all: its one unique capability is animation, and LZW plus
// interlacing is a great deal of code for a format the world has retired.

#include <stddef.h>
#include <stdint.h>

typedef enum {
    OS64_IMAGE_OK = 0,
    OS64_IMAGE_NO_FILE,          // could not open it
    OS64_IMAGE_TOO_BIG,          // file exceeds the cap — nothing decoded
    OS64_IMAGE_IO_ERROR,         // a read failed partway through
    OS64_IMAGE_NO_MEMORY,        // could not allocate
    OS64_IMAGE_UNKNOWN_FORMAT,   // magic bytes match nothing we decode
    OS64_IMAGE_MALFORMED,        // right format, wrong contents (truncated,
                                 // impossible dimensions, bad header)
    OS64_IMAGE_UNSUPPORTED       // right format, a variant we do not decode
                                 // (compressed BMP, 16-bit PPM samples) —
                                 // DISTINCT from MALFORMED on purpose: one
                                 // means the file is broken, the other means
                                 // we are incomplete, and a user deserves to
                                 // know which of those two it is
} os64_image_status_t;

typedef struct os64_image {
    uint32_t  width;
    uint32_t  height;
    uint32_t *pixels;   // 0xAARRGGBB, width*height, tightly packed.
                        // os64_malloc'd — release with os64_image_free.
} os64_image_t;

// The largest file os64_image_load will read when the caller passes cap 0.
// A cap is how a decoder declines to be a denial-of-service surface for a
// file it was handed; anything larger than this is almost certainly a
// mistake.
//
// 20MB, AND THE ARITHMETIC IS THE POINT (Codex #30 rd2 caught the old
// number). A 2048x2048 32-bit BMP is 16,777,216 bytes of raster PLUS 54
// bytes of header — so the previous 16MB cap refused, by 54 bytes, the exact
// image the comment beside it claimed was admitted "with room to spare". A
// cap stated in round binary numbers and a size stated in pixels do not
// compare themselves; someone has to multiply. The slack here is deliberate
// and covers format overhead for every variant we decode.
#define OS64_IMAGE_CAP_DEFAULT (20u * 1024u * 1024u)

// No image is allowed to claim more than this in either axis. The bound
// exists so the width*height arithmetic below cannot be walked into an
// overflow by a hostile header — every dimension check in this library is
// written to be true before the multiplication happens, not after.
#define OS64_IMAGE_DIM_MAX 16384u

// Load and decode a file. `cap` 0 means OS64_IMAGE_CAP_DEFAULT.
// On OS64_IMAGE_OK, *out owns pixels the caller frees with os64_image_free.
// On anything else *out is zeroed and owns nothing.
os64_image_status_t os64_image_load(const char *path, size_t cap,
                                    os64_image_t *out);

// Decode bytes already in memory. Same contract, no file involved — this is
// the half that is testable without a filesystem, and the half a future
// network or clipboard source would call.
os64_image_status_t os64_image_decode(const uint8_t *data, size_t len,
                                      os64_image_t *out);

// Release an image's pixels and zero it. NULL is a no-op; calling it twice
// is safe (the zeroing is what makes that true).
void os64_image_free(os64_image_t *img);

// The status as a phrase, for messages. Never NULL.
const char *os64_image_status_name(os64_image_status_t status);

#endif // OS64_IMAGE_H
