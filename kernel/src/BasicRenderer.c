#include "BasicRenderer.h"
#include "sprintf.h"
#include "strings/strlen.h"
#include "memset.h"
#include "video.h"
#include "memcpy.h"
#include "serial_logging.h"
#include "spinlock.h"
#include "kmalloc.h"   // renderer_attach_shadow — the console's RAM mirror

extern BasicRenderer kRenderer;
uint32_t kFrameBufferBackgroundColor;
volatile console_sink_fn kConsoleSink = NULL;

// The console cursor is shared mutable state, and as of husk it has more than
// one writer: the kernel task paints the uptime clock while a user task echoes
// keystrokes — on a different core. Every cursor read-modify-write in here
// (advance, wrap, scroll) is therefore a critical section; without this lock
// two cores interleave mid-string and the console garbles.
//
// irqsave (the house idiom, see spinlock.h) because print_n is reachable from
// interrupt and exception context: a plain spinlock would let an IRQ land on a
// core that already holds it and deadlock that core against itself.
//
// Cost note: a full-screen scroll (a ~3MB memmove) now runs with interrupts
// disabled, so a scroll can delay this core's tick by a few hundred µs. The
// console is not a hot path and correctness beats that jitter — but if the
// console ever gets busy enough to matter, the fix is a scroll that doesn't
// hold the lock, not a lock that doesn't cover the scroll.
static spinlock_t kRendererLock = 0;

// Panic escape hatch. If a core dies (or faults) while holding the renderer
// lock, the panic path must still reach the screen — a panic that deadlocks
// on a console lock is a panic nobody ever reads. panic() busts the lock the
// same way it detaches the GUI sink: nothing stands between a panic and the
// framebuffer. Safe by construction because after this we are not coming back.
void renderer_bust_lock(void)
{
	__sync_lock_release(&kRendererLock);
}

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

    if (basicrenderer->shadow != NULL) {
        // The fast path, and the whole reason the shadow exists: scroll in
        // RAM (memmove reads are cached and cheap), then push the finished
        // frame to VRAM with a single pure-WRITE pass. Write-combining eats
        // sequential stores at full speed; it was the READ half of the old
        // in-place memmove that cost a quarter second per scroll on metal.
        memmove(basicrenderer->shadow, basicrenderer->shadow + (16 * pixels_per_scanline), copy_bytes);
        clear_bottom_lines(basicrenderer->shadow, pixels_per_scanline, width, height - 16, height);
        memcpy(pixPtr, basicrenderer->shadow,
               (size_t)height * pixels_per_scanline * sizeof(unsigned int));
        return;
    }

    // No shadow yet (pre-kmalloc early boot): the honest slow way.
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
    basicrenderer->shadow = NULL;   // attached later, once kmalloc exists
    return;
}

void renderer_attach_shadow(void)
{
    size_t bytes = (size_t)kRenderer.framebuffer->height *
                   kRenderer.framebuffer->pixels_per_scan_line * sizeof(unsigned int);
    unsigned int *shadow = kmalloc_aligned(bytes);
    if (shadow == NULL)
        return;   // no shadow, no harm: the slow path still works

    uint64_t flags = spinlock_acquire_irqsave(&kRendererLock);
    // Seed from the live framebuffer — deliberately THE one and only VRAM
    // read the console ever performs (a few tenths of a second on metal,
    // once, at boot; from here on VRAM is write-only territory).
    memcpy(shadow, (void *)kRenderer.framebuffer->base_address, bytes);
    kRenderer.shadow = shadow;
    spinlock_release_irqrestore(&kRendererLock, flags);
}

void moveto(BasicRenderer *basicrenderer, unsigned int x, unsigned int y)
{
	uint64_t flags = spinlock_acquire_irqsave(&kRendererLock);
	basicrenderer->cursor_position.x = x * FONT_WIDTH;
	basicrenderer->cursor_position.y = y * basicrenderer->psf1_font->psf1_header->charsize;
	spinlock_release_irqrestore(&kRendererLock, flags);
}

// Read back the cursor in character cells — the inverse of moveto().
//
// NOTE: this is a snapshot, not a reservation. It is NOT a safe basis for a
// save/moveto/print/restore sequence while another core can print: the four
// steps aren't atomic, so the restore can rewind the cursor over output that
// landed in between. That is exactly why the uptime clock uses print_at()
// instead. Kept because "where is the cursor?" is a legitimate question.
void get_cursor_pos(BasicRenderer *basicrenderer, unsigned int* x, unsigned int* y)
{
	uint64_t flags = spinlock_acquire_irqsave(&kRendererLock);
	*x = basicrenderer->cursor_position.x / FONT_WIDTH;
	*y = basicrenderer->cursor_position.y / basicrenderer->psf1_font->psf1_header->charsize;
	spinlock_release_irqrestore(&kRendererLock, flags);
}

// Draw a string at an absolute character cell WITHOUT touching the shared
// console cursor — no advance, no wrap, no scroll, nothing to save or restore.
//
// This is what a STATUS WIDGET wants (the uptime clock in the top-right
// corner). A widget parks glyphs at fixed coordinates; it is not a console
// write and has no business borrowing the console's cursor. Since husk moved
// in, the console has a live tenant echoing keystrokes from another core, and
// any widget that hijacks the cursor will land its text in the middle of
// somebody's prompt.
//
// Clips at the screen edge rather than wrapping: a widget that overflows its
// corner should be truncated, never allowed to reflow the console.
void print_at(BasicRenderer *basicrenderer, unsigned int x, unsigned int y, const char *str)
{
	// If the GUI owns the console, the raw framebuffer is not ours to scribble
	// on — the compositor would just overpaint us (or we would tear its frame).
	if (kConsoleSink)
		return;

	const unsigned int charHeight = basicrenderer->psf1_font->psf1_header->charsize;
	unsigned int px = x * FONT_WIDTH;
	unsigned int py = y * charHeight;

	// Same lock as print_n: we don't touch the cursor, but we DO share the
	// framebuffer — drawing into a scroll-in-progress would tear.
	uint64_t flags = spinlock_acquire_irqsave(&kRendererLock);
	for (const char *chr = str; *chr; chr++)
	{
		if (px + FONT_WIDTH > basicrenderer->framebuffer->width)
			break;
		if (py + charHeight > basicrenderer->framebuffer->height)
			break;
		put_char(basicrenderer, *chr, px, py);
		px += FONT_WIDTH;
	}
	spinlock_release_irqrestore(&kRendererLock, flags);
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

    // Hold the lock for the WHOLE string, not per character: the point is that
    // two concurrent printers must not interleave their glyphs (or, worse, have
    // one core's wrap/scroll relocate the cursor out from under the other's
    // next put_char). One print_n call == one atomic console write.
    uint64_t flags = spinlock_acquire_irqsave(&kRendererLock);
    for (size_t i = 0; i < length; i++, chr++) {
        switch (*chr) {
            case '\n':
                basicrenderer->cursor_position.x = 0;
                basicrenderer->cursor_position.y += FONT_HEIGHT;
                break;
            case '\t':
                basicrenderer->cursor_position.x += FONT_WIDTH;
                break;
            case '\b':
                // Move back one cell, clamped at the line start (a terminal
                // never backspaces up a line). This only MOVES the cursor —
                // erasure is the caller's job by overprinting, which is why
                // husk rubs out a glyph with "\b \b": back, blank, back.
                if (basicrenderer->cursor_position.x >= FONT_WIDTH)
                    basicrenderer->cursor_position.x -= FONT_WIDTH;
                break;
            case '\r':
                // Carriage return does what the carriage did: column zero,
                // same line. (Previously fell through to default and drew a
                // garbage glyph.)
                basicrenderer->cursor_position.x = 0;
                break;
            default:
                put_char(basicrenderer, *chr, basicrenderer->cursor_position.x, basicrenderer->cursor_position.y);
                basicrenderer->cursor_position.x += FONT_WIDTH;
                break;
        }

        // Handle line wrapping
        if (basicrenderer->cursor_position.x + FONT_WIDTH > basicrenderer->framebuffer->width) {
            basicrenderer->cursor_position.x = 0;
            basicrenderer->cursor_position.y += FONT_HEIGHT;
        }

        // Handle scrolling
        if (basicrenderer->cursor_position.y + FONT_HEIGHT > basicrenderer->framebuffer->height) {
            scroll_framebuffer_full(basicrenderer);
            basicrenderer->cursor_position.y = basicrenderer->framebuffer->height - FONT_HEIGHT;
        }
    }
    spinlock_release_irqrestore(&kRendererLock, flags);
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

            // One store to VRAM, one to the shadow (writes only, both) —
            // the mirror must stay pixel-true or the next scroll's blit
            // would repaint the screen with stale glyphs.
            unsigned int px = ((*fontPtr & (0b10000000 >> (x - xOff))) > 0)
                                  ? basicrenderer->color
                                  : kFrameBufferBackgroundColor;
            size_t idx = x + (y * basicrenderer->framebuffer->pixels_per_scan_line);
            *(pixPtr + idx) = px;
            if (basicrenderer->shadow != NULL)
                basicrenderer->shadow[idx] = px;
        }
        fontPtr++;
    }
}

void clear(BasicRenderer *basicrenderer, uint32_t color, bool resetCursor)
{
    uint64_t fbBase = (uint64_t)basicrenderer->framebuffer->base_address;
    uint64_t pxlsPerScanline = basicrenderer->framebuffer->pixels_per_scan_line;

    // Wiping the screen and homing the cursor is the most destructive thing the
    // renderer does — it must not land in the middle of another core's string.
    uint64_t flags = spinlock_acquire_irqsave(&kRendererLock);

    for (int64_t y = 0; y < basicrenderer->framebuffer->height; y++)
    {
        for (int64_t x = 0; x < basicrenderer->framebuffer->width; x++)
        {
            *((uint32_t *)(fbBase + 4 * (x + pxlsPerScanline * y))) = color;
            if (basicrenderer->shadow != NULL)
                basicrenderer->shadow[x + pxlsPerScanline * y] = color;
        }
    }

    if (resetCursor)
    {
        basicrenderer->cursor_position.x = 0;
        basicrenderer->cursor_position.y = 0;
    }

	kFrameBufferBackgroundColor = color;

    spinlock_release_irqrestore(&kRendererLock, flags);
    return;
}
