#include "BasicRenderer.h"
#include "sprintf.h"
#include "strings/strlen.h"
#include "memset.h"
#include "video.h"
#include "memcpy.h"
#include "serial_logging.h"
#include "spinlock.h"
#include "kmalloc.h"   // renderer_attach_shadow — the console's RAM mirror
#include "tty.h"       // print_n's router half: grids up -> bytes go to VT1
#include "gui/compositor.h"  // gui_owns_glass — "is the iron the GUI's right now?"

extern BasicRenderer kRenderer;
uint32_t kFrameBufferBackgroundColor;
// (kConsoleSink lived here from the first GUI console until the VT8 chapter
// retired it, 2026-08-19: a diversion at the bottom of this renderer that
// caught the tty layer's glass paints in a net and fed them to a ring-0
// window. Glass ownership is VT focus now — gui_owns_glass() — and the only
// question this file asks is whether the iron is currently the GUI's.)

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

// ── Blit throttle (2026-08-04, the console-speed slice, QEMU half) ──────────
// A scroll used to end in a full shadow→glass blit — ~3MB — EVERY time. At
// DEBUG_SCHEDULER volume that's thousands of blits a second painting frames
// no eye can distinguish. The throttle: the SHADOW always updates instantly
// (it is the truth), but the glass repaints at most once per BLIT_MIN_TICKS
// (~30Hz). A burst of 100 scrolled lines = 3 blits instead of 100; the cost
// is at most ~30ms of display lag, below human notice. While the glass is
// behind (s_glassDirty), put_char skips its VRAM store too — those pixels
// get repainted by the pending flush anyway. The flush itself rides
// processSignals (renderer_flush_if_dirty below), so a burst's tail lands
// within a tick of the burst ending. All throttle state is guarded by
// kRendererLock (or by the panic path, which busts that lock first).
static bool s_glassDirty = false;        // shadow is ahead of the glass
static uint64_t s_lastBlitTick = 0;
static bool s_throttleEnabled = true;    // false forever after a panic
#define BLIT_MIN_TICKS 3                 // 3 ticks = ~30ms = ~30Hz max
extern volatile uint64_t kTicksSinceStart;
extern BasicRenderer kRenderer;

// The one true blit, shared by scroll, the flush rider, and the panic path.
static void renderer_blit_full(BasicRenderer *r)
{
	if (r->shadow == NULL)
		return;
	memcpy((unsigned int *)r->framebuffer->base_address, r->shadow,
	       (size_t)r->framebuffer->height *
	           r->framebuffer->pixels_per_scan_line * sizeof(unsigned int));
	s_lastBlitTick = kTicksSinceStart;
	s_glassDirty = false;
}

// Panic escape hatch. If a core dies (or faults) while holding the renderer
// lock, the panic path must still reach the screen — a panic that deadlocks
// on a console lock is a panic nobody ever reads. panic() busts the lock the
// same way it detaches the GUI sink: nothing stands between a panic and the
// framebuffer. Safe by construction because after this we are not coming back.
//
// THROTTLE OVERRIDE: the blit throttle above must die here too — a panic
// message written to a shadow that never flushes is a panic nobody reads,
// the same sin with a newer name. Disable throttling forever and force the
// glass current, so every subsequent panic print lands immediately.
void renderer_bust_lock(void)
{
	__sync_lock_release(&kRendererLock);
	s_throttleEnabled = false;
	renderer_blit_full(&kRenderer);
}

// The flush rider (called from processSignals): if a burst left the glass
// behind and the throttle window has passed, push the finished frame. The
// unlocked s_glassDirty peek is safe — a stale read costs one pass, and the
// locked recheck decides for real.
void renderer_flush_if_dirty(void)
{
	if (!s_glassDirty)
		return;
	uint64_t flags = spinlock_acquire_irqsave(&kRendererLock);
	if (s_glassDirty && kTicksSinceStart - s_lastBlitTick >= BLIT_MIN_TICKS)
		renderer_blit_full(&kRenderer);
	spinlock_release_irqrestore(&kRendererLock, flags);
}

// NOTE: this file is the LEGACY text console — direct-to-framebuffer, no
// windowing. The GUI subsystem (kernel/src/gui/) renders through its own
// surface engine and, when active, will divert print_n() into a console
// window. The old half-finished shadow-buffer/MTRR experiments that lived
// here were removed when the GUI's backbuffer superseded them.

// ── The cursor (2026-08-08 — "need a cursor! :-)") ──────────────────────────
// A solid underscore at the console cursor cell, shown by console_read while
// a reader is parked awaiting keys and hidden the moment anything else needs
// the glass. The discipline that keeps it honest with one flag and no timer:
//
//   - console_read SHOWS it before parking and HIDES it on every exit path,
//     so it glows exactly while the machine is listening — the semantics a
//     terminal cursor has had since the VT05 grew one in 1970.
//   - EVERY print path HIDES it first (print_n's first act under the lock),
//     because the cursor may be sitting mid-line over a real glyph during
//     line editing, and output must never land on (or scroll away from
//     under) a painted cursor.
//   - Show SAVES the pixels it covers and hide RESTORES them — the cursor
//     works over any cell content without the renderer needing to know what
//     a "character" is. clear()/'\f' DROP the saved pixels instead of
//     restoring (the screen they came from is gone).
//
// All state below is guarded by kRendererLock. No blink: blinking needs a
// periodic agent, and the parked reader is by definition not running — a
// steady underscore is the honest v1 (and what the VT05 shipped, for the
// same reason).
#define CURSOR_ROWS 2                       // bottom 2 pixel rows of the cell
static bool s_cursorOn = false;
static unsigned int s_cursorPX, s_cursorPY;              // cell origin, pixels
static unsigned int s_cursorSave[CURSOR_ROWS * FONT_WIDTH];

// Caller holds kRendererLock. Restore what the cursor covered.
static void cursor_hide_locked(BasicRenderer *r)
{
	if (!s_cursorOn)
		return;
	s_cursorOn = false;
	unsigned int *pixPtr = (unsigned int *)r->framebuffer->base_address;
	unsigned int charH = r->psf1_font->psf1_header->charsize;
	for (unsigned int row = 0; row < CURSOR_ROWS; row++)
	{
		unsigned int y = s_cursorPY + charH - CURSOR_ROWS + row;
		for (unsigned int x = 0; x < FONT_WIDTH; x++)
		{
			if (s_cursorPX + x >= r->framebuffer->width || y >= r->framebuffer->height)
				continue;
			size_t idx = (s_cursorPX + x) + (size_t)y * r->framebuffer->pixels_per_scan_line;
			unsigned int px = s_cursorSave[row * FONT_WIDTH + x];
			if (!s_glassDirty)
				*(pixPtr + idx) = px;
			if (r->shadow != NULL)
				r->shadow[idx] = px;
		}
	}
}

// Caller holds kRendererLock. Save the cell's true pixels, paint the bar.
static void cursor_show_locked(BasicRenderer *r)
{
	if (s_cursorOn)
		cursor_hide_locked(r);   // repaint at the CURRENT position
	unsigned int *pixPtr = (unsigned int *)r->framebuffer->base_address;
	unsigned int charH = r->psf1_font->psf1_header->charsize;
	s_cursorPX = r->cursor_position.x;
	s_cursorPY = r->cursor_position.y;
	for (unsigned int row = 0; row < CURSOR_ROWS; row++)
	{
		unsigned int y = s_cursorPY + charH - CURSOR_ROWS + row;
		for (unsigned int x = 0; x < FONT_WIDTH; x++)
		{
			if (s_cursorPX + x >= r->framebuffer->width || y >= r->framebuffer->height)
				continue;
			size_t idx = (s_cursorPX + x) + (size_t)y * r->framebuffer->pixels_per_scan_line;
			// Save from the SHADOW when there is one — it is the truth even
			// while the glass is throttle-dirty; VRAM is never read after boot.
			s_cursorSave[row * FONT_WIDTH + x] =
			    (r->shadow != NULL) ? r->shadow[idx] : *(pixPtr + idx);
			if (!s_glassDirty)
				*(pixPtr + idx) = r->color;
			if (r->shadow != NULL)
				r->shadow[idx] = r->color;
		}
	}
	s_cursorOn = true;
}

// The console_read entry points (console.h): show while listening, hide when
// anything else happens. GUI-diverted consoles paint their own cursor someday.
void renderer_cursor_show(void)
{
	// The desktop is not ours to blink on: a husk parked in console_read on
	// VT1 must not paint its cursor over the GUI's frame.
	if (gui_owns_glass())
		return;
	uint64_t flags = spinlock_acquire_irqsave(&kRendererLock);
	cursor_show_locked(&kRenderer);
	spinlock_release_irqrestore(&kRendererLock, flags);
}

void renderer_cursor_hide(void)
{
	if (gui_owns_glass())
		return;
	uint64_t flags = spinlock_acquire_irqsave(&kRendererLock);
	cursor_hide_locked(&kRenderer);
	spinlock_release_irqrestore(&kRendererLock, flags);
}

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
        // THROTTLED (see the doctrine above kRendererLock): blit only when
        // the ~30Hz window allows; otherwise mark the glass dirty and let
        // the processSignals rider deliver the finished frame. A log burst
        // scrolls the shadow a hundred times and the glass thrice.
        (void)pixPtr;
        if (!s_throttleEnabled ||
            kTicksSinceStart - s_lastBlitTick >= BLIT_MIN_TICKS)
            renderer_blit_full(basicrenderer);
        else
            s_glassDirty = true;
        return;
    }

    // No shadow yet (pre-kmalloc early boot): the honest slow way.
    // Move all lines up by FONT_HEIGHT (16 pixels)
    memmove(pixPtr, pixPtr + (16 * pixels_per_scanline), copy_bytes);

    // Clear the last FONT_HEIGHT lines
	clear_bottom_lines(pixPtr, pixels_per_scanline, width, height - 16, height);
}

// Wipe the whole console to the background color — the screen half of form
// feed ('\f', 0x0C: "eject the page" on a teletype; "wipe the glass" ever
// since screens replaced paper). Caller holds kRendererLock (print_n does).
//
// Same shadow discipline as scroll_framebuffer_full: fill in RAM, then push
// one pure-WRITE frame to VRAM. Cursor homing is the caller's job — this
// function only owns the pixels.
static void clear_framebuffer_full(BasicRenderer *basicrenderer)
{
    unsigned int *pixPtr = (unsigned int *)basicrenderer->framebuffer->base_address;
    unsigned int pixels_per_scanline = basicrenderer->framebuffer->pixels_per_scan_line;
    unsigned int width = basicrenderer->framebuffer->width;
    unsigned int height = basicrenderer->framebuffer->height;

    if (basicrenderer->shadow != NULL) {
        clear_bottom_lines(basicrenderer->shadow, pixels_per_scanline, width, 0, height);
        memcpy(pixPtr, basicrenderer->shadow,
               (size_t)height * pixels_per_scanline * sizeof(unsigned int));
        return;
    }

    // No shadow yet (pre-kmalloc early boot): paint VRAM directly.
    clear_bottom_lines(pixPtr, pixels_per_scanline, width, 0, height);
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
	// If the GUI owns the glass, the raw framebuffer is not ours to scribble
	// on — the compositor would just overpaint us (or we would tear its frame).
	if (gui_owns_glass())
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

// Length-bounded console output — the worker behind print(). Since the
// virtual-terminal slice this is a ROUTER: once tty_init has built the grids,
// every printf/print lands in VT1's grid (the system console — the BSD
// tradition: kernel messages have an address, and it is terminal one). The
// direct interpreter below survives for its two remaining customers — early
// boot and panic.
void print_n(const char* str, size_t length) {
    // (The kConsoleSink diversion stood here until 2026-08-19. Nothing
    // replaces it: with the GUI on VT8, these bytes belong in VT1's grid
    // below — visible the moment anyone switches — and tty_write's own
    // focused-check keeps them off a glass the compositor owns.)
    if (kTTYReady && !kTTYDirect) {
        tty_write(&kTTY[0], str, length);
        return;
    }

    print_n_direct(str, length);
}

// The legacy direct-to-glass interpreter (contract in BasicRenderer.h). This
// is the ancestral print_n body, byte for byte; tty.c's interpreter is its
// grid-backed descendant and MUST keep these exact semantics (including the
// one-cell '\t' quirk) — the two must never be allowed to drift, which is
// why the tty version carries the same comments at the same decisions.
void print_n_direct(const char* str, size_t length) {
    const char *chr = str;
    BasicRenderer *basicrenderer = &kRenderer;

    // Hold the lock for the WHOLE string, not per character: the point is that
    // two concurrent printers must not interleave their glyphs (or, worse, have
    // one core's wrap/scroll relocate the cursor out from under the other's
    // next put_char). One print_n call == one atomic console write.
    uint64_t flags = spinlock_acquire_irqsave(&kRendererLock);
    // First act under the lock: the cursor gets out of the way (restoring the
    // pixels it covered), so no glyph, wrap, or scroll ever lands on it.
    cursor_hide_locked(basicrenderer);
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
            case '\f':
                // Form feed does what the form feed did: fresh page. Wipe
                // the surface, home the cursor. Interpreted HERE, at the
                // glass, on purpose: through a pipe or into a file '\f' is
                // just a data byte, and whichever console eventually drains
                // it clears ITS OWN surface — clear(1) needs no syscall and
                // no knowledge of which screen it lands on.
                clear_framebuffer_full(basicrenderer);
                basicrenderer->cursor_position.x = 0;
                basicrenderer->cursor_position.y = 0;
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

// The glyph blitter, with BOTH colors named. put_char is this with the
// console's one background color, which is what it always used; the explicit
// form exists because a cell sometimes needs its own background — inverse
// video for a text-console selection is the first customer (vt_select.c,
// 2026-08-21), and gpm's highlight on the Linux console was exactly this
// operation. Any future per-cell attribute lands here too.
static void put_char_colors(BasicRenderer *basicrenderer, char chr,
                            unsigned int xOff, unsigned int yOff,
                            uint32_t fg, uint32_t bg)
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
            // would repaint the screen with stale glyphs. EXCEPT while the
            // glass is throttle-dirty: then the pending full blit will
            // repaint every pixel anyway, so the VRAM store is skipped and
            // a burst's glyph rendering costs shadow writes only.
            unsigned int px = ((*fontPtr & (0b10000000 >> (x - xOff))) > 0)
                                  ? fg
                                  : bg;
            size_t idx = x + (y * basicrenderer->framebuffer->pixels_per_scan_line);
            if (!s_glassDirty)
                *(pixPtr + idx) = px;
            if (basicrenderer->shadow != NULL)
                basicrenderer->shadow[idx] = px;
        }
        fontPtr++;
    }
}

void put_char(BasicRenderer *basicrenderer, char chr, unsigned int xOff, unsigned int yOff)
{
    put_char_colors(basicrenderer, chr, xOff, yOff,
                    basicrenderer->color, kFrameBufferBackgroundColor);
}

// ── The dumb-glass API (2026-08-08 — the renderer's demotion papers) ────────
// Contract in BasicRenderer.h: tty.c owns the terminal logic, these own the
// paint. begin/end bracket one atomic glass session under kRendererLock —
// the same whole-string atomicity print_n always enforced, now enforced by
// the caller because the caller is the one who knows where a write ends.

uint32_t renderer_cols(void)
{
	return kRenderer.framebuffer->width / FONT_WIDTH;
}

uint32_t renderer_rows(void)
{
	return kRenderer.framebuffer->height / FONT_HEIGHT;
}

uint64_t renderer_glass_begin(void)
{
	uint64_t flags = spinlock_acquire_irqsave(&kRendererLock);
	// First act, same as print_n's: the cursor gets out of the way
	// (restoring the pixels it covered) before any glyph or scroll lands.
	cursor_hide_locked(&kRenderer);
	return flags;
}

void renderer_glass_end(uint64_t flags, uint32_t row, uint32_t col, bool show_cursor)
{
	// Park the pixel cursor at the tty's cell so the underscore cursor (and
	// any get_cursor_pos snapshot) agrees with the grid about where we are.
	kRenderer.cursor_position.x = col * FONT_WIDTH;
	kRenderer.cursor_position.y = row * FONT_HEIGHT;
	if (show_cursor)
		cursor_show_locked(&kRenderer);
	spinlock_release_irqrestore(&kRendererLock, flags);
}

void renderer_glass_putc_locked(char ch, uint32_t row, uint32_t col, uint32_t color)
{
	// The tty's per-cell color becomes the renderer color for this glyph —
	// nothing else writes kRenderer.color at runtime (checked 2026-08-08),
	// so there is nothing to save and restore.
	kRenderer.color = color;
	put_char(&kRenderer, ch, col * FONT_WIDTH, row * FONT_HEIGHT);
}

void renderer_glass_putc_bg_locked(char ch, uint32_t row, uint32_t col,
                                   uint32_t fg, uint32_t bg)
{
	// Same contract as the putc above, with the background named. Does NOT
	// touch kRenderer.color: an overlay is a temporary lie about one cell,
	// and the tty's idea of the current write color must survive it intact.
	put_char_colors(&kRenderer, ch, col * FONT_WIDTH, row * FONT_HEIGHT, fg, bg);
}

void renderer_glass_scroll_locked(void)
{
	scroll_framebuffer_full(&kRenderer);
}

void renderer_glass_clear_locked(void)
{
	clear_framebuffer_full(&kRenderer);
}

void renderer_glass_defer_locked(void)
{
	// Only meaningful with a shadow to defer INTO: pre-shadow, put_char
	// writes VRAM directly and a "deferred" glyph would simply vanish.
	// (Moot in practice — tty_init runs after renderer_attach_shadow —
	// but a repaint must never be able to paint a black screen.)
	if (kRenderer.shadow != NULL)
		s_glassDirty = true;
}

void renderer_glass_blit_locked(void)
{
	renderer_blit_full(&kRenderer);
}

void clear(BasicRenderer *basicrenderer, uint32_t color, bool resetCursor)
{
    uint64_t fbBase = (uint64_t)basicrenderer->framebuffer->base_address;
    uint64_t pxlsPerScanline = basicrenderer->framebuffer->pixels_per_scan_line;

    // Wiping the screen and homing the cursor is the most destructive thing the
    // renderer does — it must not land in the middle of another core's string.
    uint64_t flags = spinlock_acquire_irqsave(&kRendererLock);

    // The text cursor's saved pixels describe a screen that is about to stop
    // existing — drop it, don't restore it.
    s_cursorOn = false;

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
