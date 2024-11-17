#include "BasicRenderer.h"
#include "sprintf.h"
#include "memset.h"
#include "video.h"

extern BasicRenderer kRenderer;
uint32_t kFrameBufferBackgroundColor;

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
	basicrenderer->cursor_position.x = x;
	basicrenderer->cursor_position.y = y * 16;
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

void print(const char* str)
{
    char *chr = (char *)str;
	BasicRenderer *basicrenderer = &kRenderer;
    while (*chr != 0)
    {
        switch (*chr)
        {
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

        if (basicrenderer->cursor_position.x + 8 > basicrenderer->framebuffer->width)
        {
            basicrenderer->cursor_position.x = 0;
            basicrenderer->cursor_position.y += 16;
        }

        chr++;
    }

    return;
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
