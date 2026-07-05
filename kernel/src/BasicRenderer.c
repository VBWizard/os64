#include "BasicRenderer.h"
#include "sprintf.h"
#include "strings/strlen.h"
#include "memset.h"
#include "video.h"
#include "memcpy.h"
#include "serial_logging.h"

extern BasicRenderer kRenderer;
uint32_t kFrameBufferBackgroundColor;
volatile console_sink_fn kConsoleSink = NULL;

// NOTE: this file is the LEGACY text console — direct-to-framebuffer, no
// windowing. The GUI subsystem (kernel/src/gui/) renders through its own
// surface engine and, when active, will divert print_n() into a console
// window. The old half-finished shadow-buffer/MTRR experiments that lived
// here were removed when the GUI's backbuffer superseded them.

void clear_bottom_lines(unsigned int *pixPtr, unsigned int pixels_per_scanline, unsigned int width, unsigned int start_line, unsigned int end_line) {
    // 32-bit fill, NOT memset: memset replicates a single byte, which only
    // painted the right color when all four background-color bytes happened
    // to match (i.e. black).
    for (unsigned int line = start_line; line < end_line; line++) {
        unsigned int *row = pixPtr + (line * pixels_per_scanline);
        for (unsigned int x = 0; x < width; x++)
            row[x] = kFrameBufferBackgroundColor;
    }
}

void scroll_framebuffer_full(BasicRenderer *basicrenderer) {
    unsigned int *pixPtr = (unsigned int *)basicrenderer->framebuffer->base_address;
    unsigned int pixels_per_scanline = basicrenderer->framebuffer->pixels_per_scan_line;
    unsigned int width = basicrenderer->framebuffer->width;
    unsigned int height = basicrenderer->framebuffer->height;

    size_t visible_lines = height - 16; // Height minus one font line
    size_t copy_bytes = visible_lines * pixels_per_scanline * sizeof(unsigned int);

    // Move all lines up by FONT_HEIGHT (16 pixels)
    memmove(pixPtr, pixPtr + (16 * pixels_per_scanline), copy_bytes);

    // Clear the last FONT_HEIGHT lines
	clear_bottom_lines(pixPtr, pixels_per_scanline, width, height - 16, height);
}

void init_renderer(BasicRenderer *basicrenderer, struct Framebuffer *framebuffer, struct PSF1_FONT *psf1_font)
{
    basicrenderer->color = 0xffffffff;

    basicrenderer->cursor_position.x = 0;
    basicrenderer->cursor_position.y = 0;

    basicrenderer->framebuffer = framebuffer;
    basicrenderer->psf1_font = psf1_font;
    return;
}

void moveto(BasicRenderer *basicrenderer, unsigned int x, unsigned int y)
{
	basicrenderer->cursor_position.x = x * 8;
	basicrenderer->cursor_position.y = y * basicrenderer->psf1_font->psf1_header->charsize;
}

int printf(const char *fmt, ...)
{
	char printf_buf[1024];
	va_list args;
	int printed;

	memset(printf_buf,0,1024);
	va_start(args, fmt);
	printed = vsprintf(printf_buf, fmt, args);
	va_end(args);
	print(printf_buf);
	return printed;
}

// Length-bounded console output — the worker behind print().  Exists so the
// write() syscall can push exact byte counts (which may legally contain NUL
// bytes) through the same cursor/wrap/scroll logic instead of duplicating it.
void print_n(const char* str, size_t length) {
    // GUI console diversion (see kConsoleSink in BasicRenderer.h). Snapshot
    // the pointer once: panic may NULL it concurrently, and we must not call
    // through a pointer we haven't checked.
    console_sink_fn sink = kConsoleSink;
    if (sink) {
        sink(str, length);
        return;
    }

    const char *chr = str;
    BasicRenderer *basicrenderer = &kRenderer;
    for (size_t i = 0; i < length; i++, chr++) {
        switch (*chr) {
            case '\n':
                basicrenderer->cursor_position.x = 0;
                basicrenderer->cursor_position.y += 16;
                break;
            case '\t':
                basicrenderer->cursor_position.x += 8;
                break;
            default:
                put_char(basicrenderer, *chr, basicrenderer->cursor_position.x, basicrenderer->cursor_position.y);
                basicrenderer->cursor_position.x += 8;
                break;
        }

        // Handle line wrapping
        if (basicrenderer->cursor_position.x + 8 > basicrenderer->framebuffer->width) {
            basicrenderer->cursor_position.x = 0;
            basicrenderer->cursor_position.y += 16;
        }

        // Handle scrolling
        if (basicrenderer->cursor_position.y + 16 > basicrenderer->framebuffer->height) {
            scroll_framebuffer_full(basicrenderer);
            basicrenderer->cursor_position.y = basicrenderer->framebuffer->height - 16;
        }
    }
}

void print(const char* str) {
    print_n(str, strlen(str));
}

void put_char(BasicRenderer *basicrenderer, char chr, unsigned int xOff, unsigned int yOff)
{
    unsigned int *pixPtr = (unsigned int *)basicrenderer->framebuffer->base_address;
    char *fontPtr = (char *)basicrenderer->psf1_font->glyph_buffer + (chr * basicrenderer->psf1_font->psf1_header->charsize);

    for (unsigned long y = yOff; y < yOff + 16; y++)
    {
        for (unsigned long x = xOff; x < xOff + 8; x++)
        {
            if (x >= basicrenderer->framebuffer->width || y >= basicrenderer->framebuffer->height)
                continue;

            if ((*fontPtr & (0b10000000 >> (x - xOff))) > 0) {
                *(pixPtr + x + (y * basicrenderer->framebuffer->pixels_per_scan_line)) = basicrenderer->color;
            } else {
                *(pixPtr + x + (y * basicrenderer->framebuffer->pixels_per_scan_line)) = kFrameBufferBackgroundColor;
            }
        }
        fontPtr++;
    }
}

void clear(BasicRenderer *basicrenderer, uint32_t color, bool resetCursor)
{
    uint64_t fbBase = (uint64_t)basicrenderer->framebuffer->base_address;
    uint64_t pxlsPerScanline = basicrenderer->framebuffer->pixels_per_scan_line;

    for (int64_t y = 0; y < basicrenderer->framebuffer->height; y++)
    {
        for (int64_t x = 0; x < basicrenderer->framebuffer->width; x++)
        {
            *((uint32_t *)(fbBase + 4 * (x + pxlsPerScanline * y))) = color;
        }
    }

    if (resetCursor)
    {
        basicrenderer->cursor_position.x = 0;
        basicrenderer->cursor_position.y = 0;
    }

	kFrameBufferBackgroundColor = color;
    return;
}
