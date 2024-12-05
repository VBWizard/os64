#include "BasicRenderer.h"
#include "sprintf.h"
#include "memset.h"
#include "video.h"
#include "memcpy.h"
#include "serial_logging.h"
#include "kmalloc.h"
#include "msr.h"
#include "paging.h"

extern BasicRenderer kRenderer;
uint32_t kFrameBufferBackgroundColor;
bool frameBufferRequiresUpdate = false;

void update_framebuffer_from_shadow()
{
#ifdef ENABLE_DOUBLE_BUFFER
	printd(DEBUG_BOOT,"Start framebuffer update\n");
	//memcpy(kRenderer.framebuffer->base_address, kRenderer.shadow_buffer, kRenderer.framebuffer->buffer_size);
	uintptr_t* src = (uintptr_t*)kRenderer.shadow_buffer;
	uintptr_t* dest = (uintptr_t*)kRenderer.framebuffer->base_address;
	// for (size_t cnt=0;cnt<kRenderer.framebuffer->buffer_size;cnt+=sizeof(uintptr_t))
	// 	*dest++ = *src++;
	for (size_t i = 0; i < kRenderer.framebuffer->buffer_size / sizeof(uintptr_t) / 4; i++) {
    	dest[i] = src[i];
}
//__asm__ volatile ("sfence" ::: "memory"); // Serialize writes
	printd(DEBUG_BOOT,"Executed sfence\n");
#endif
}

void clear_bottom_lines(unsigned int *pixPtr, unsigned int pixels_per_scanline, unsigned int width, unsigned int start_line, unsigned int end_line) {
    size_t clear_bytes = (end_line - start_line) * width * sizeof(unsigned int);
    unsigned int *start = pixPtr + (start_line * pixels_per_scanline);
    memset(start, kFrameBufferBackgroundColor, clear_bytes);
}

void scroll_framebuffer_full(BasicRenderer *basicrenderer) {
#ifdef ENABLE_DOUBLE_BUFFER
    unsigned int *pixPtr = (unsigned int *)basicrenderer->shadow_buffer;
#else
    unsigned int *pixPtr = (unsigned int *)basicrenderer->framebuffer->base_address;
#endif
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

void scroll_framebuffer(BasicRenderer *basicrenderer) {
#ifdef ENABLE_DOUBLE_BUFFER
    unsigned int *pixPtr = (unsigned int *)basicrenderer->shadow_buffer;
#else
    unsigned int *pixPtr = (unsigned int *)basicrenderer->framebuffer->base_address;
#endif
    unsigned int pixels_per_scanline = basicrenderer->framebuffer->pixels_per_scan_line;
    unsigned int width = basicrenderer->framebuffer->width;
    unsigned int height = basicrenderer->framebuffer->height;

	for (int cnt=0;cnt<16;cnt++)
    // Adjusted loop to include the last line
    for (unsigned int y = 0; y < height - 1; y++) {
        unsigned int *source = pixPtr + ((y + 1) * pixels_per_scanline);
        unsigned int *destination = pixPtr + (y * pixels_per_scanline);
        memmove(destination, source, width * sizeof(unsigned int)); // Move only the visible pixels
    }

    // Clear the bottom line
    unsigned int *lastLine = pixPtr + ((height - 1) * pixels_per_scanline);
    for (unsigned int x = 0; x < width; x++) {
        lastLine[x] = kFrameBufferBackgroundColor; // Clear to background color
    }

}

// Round up to the nearest power of two
uint64_t round_up_power_of_two(uint64_t size) {
    uint64_t power = 1;
    while (power < size) {
        power <<= 1;
    }
    return power;
}

// Set up an MTRR for write-combining
void setup_mtrr_write_combine(uint64_t framebuffer_base, uint64_t framebuffer_size) {
    // Round framebuffer size up to nearest power of two
    uint64_t range_size = round_up_power_of_two(framebuffer_size);

    // Verify alignment of the base address to the range size
    if (framebuffer_base % range_size != 0) {
        printf("Error: Framebuffer base address is not aligned to range size.\n");
        return;
    }

    // Set the MTRR base address with WC type (6)
    uint64_t mtrr_base = framebuffer_base | 0x06; // WC = 6

    // Set the MTRR mask with range size and enable bit
    uint64_t mtrr_mask = ~(range_size - 1) & 0xFFFFFFFFFFFFF000ULL; // Mask for range size
    mtrr_mask |= 0x800; // Enable bit

    // Write to MTRR base (MSR 0x200) and mask (MSR 0x201)
    wrmsr64(0x200, mtrr_base);
    wrmsr64(0x201, mtrr_mask);

    printf("MTRR configured: Base=0x%llx, Size=0x%llx\n", framebuffer_base, range_size);
}

void init_renderer(BasicRenderer *basicrenderer, struct Framebuffer *framebuffer, struct PSF1_FONT *psf1_font)
{
    basicrenderer->color = 0xffffffff;

    basicrenderer->cursor_position.x = 0;
    basicrenderer->cursor_position.y = 0;

    basicrenderer->framebuffer = framebuffer;
    basicrenderer->psf1_font = psf1_font;
#ifdef ENABLE_DOUBLE_BUFFER
	basicrenderer->shadow_buffer = kmalloc_aligned(basicrenderer->framebuffer->buffer_size);
	memcpy(basicrenderer->shadow_buffer, basicrenderer->framebuffer->base_address, basicrenderer->framebuffer->buffer_size);
#endif
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

void print(const char* str) {
    const char *chr = str;
    BasicRenderer *basicrenderer = &kRenderer;
    while (*chr != 0) {
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

        chr++;
    }
}

void put_char(BasicRenderer *basicrenderer, char chr, unsigned int xOff, unsigned int yOff)
{
#ifdef ENABLE_DOUBLE_BUFFER
    unsigned int *pixPtr = (unsigned int *)basicrenderer->shadow_buffer;
#else
    unsigned int *pixPtr = (unsigned int *)basicrenderer->framebuffer->base_address;
#endif
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
	frameBufferRequiresUpdate = true;
}

void clear(BasicRenderer *basicrenderer, uint32_t color, bool resetCursor)
{
#ifdef ENABLE_DOUBLE_BUFFER
    uint64_t fbBase = (uint64_t)basicrenderer->shadow_buffer;
#else
    uint64_t fbBase = (uint64_t)basicrenderer->framebuffer->base_address;
#endif
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
