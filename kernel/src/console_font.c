// console_font.c — see console_font.h for the two halves and why they are two.

#include "console_font.h"

#include <stdarg.h>
#include "BasicRenderer.h"
#include "psf2.h"
#include "tty.h"
#include "task.h"
#include "scheduler.h"         // kTaskList — the seat walk, as pty_resize does it
#include "signals.h"
#include "spinlock.h"
#include "kmalloc.h"
#include "memcpy.h"
#include "memset.h"
#include "printd.h"
#include "video.h"
#include "CONFIG.h"
#include "strings/sprintf.h"
#include "os64/charset.h"

extern task_t *kKernelTask;

// A GRID ANYBODY COULD WORK AT. The loader's fences are about memory; these
// are about the person. A 64x128 face on a 1024x768 screen is a valid font
// and a 16x6 terminal, which is a prompt and nowhere to read its answer —
// the refusal is cheaper than the reboot it would take to undo. The upper
// pair is tty_refont's own fence (tty_refont_limits), asked at submit so the
// refusal has a reason.
#define CONSOLE_GRID_COLS_MIN  40u
#define CONSOLE_GRID_ROWS_MIN  10u

// The blocks psf2_synth_block can make, in the order their slots are laid
// out after the blank one. ANSI art cannot be drawn without them, so a face
// that lacks one gets it made to measure instead of a hole.
static const uint32_t kSynthBlocks[] = {
	0x2580, 0x2584, 0x2588, 0x258C, 0x2590, 0x2591, 0x2592, 0x2593,
};
#define SYNTH_COUNT (sizeof(kSynthBlocks) / sizeof(kSynthBlocks[0]))

// A face that has been validated and resolved, and everything it owns.
typedef struct
{
	uint8_t *image;               // the PSF2 bytes; face.glyphs points into it
	const uint8_t **resolved;     // 2 x 256 bitmap pointers (BasicRenderer.h)
	uint8_t *extra;               // slot 0 blank, then SYNTH_COUNT made blocks
	console_face_t face;
	uint32_t cols, rows;          // the grid this face gives this screen
	bool from_table;
	uint32_t unmapped[2];
} console_loaded_t;

// ONE OPEN FILE CAN HAVE TWO WRITERS: spawn redirection shares a file object
// between parent and child, and a handle's pin protects the object's
// lifetime, not the order of its writes. Two appends that both decide to grow
// would each copy, swap and free the same buffer, which is not interleaved
// file contents but a double free in the kernel heap. So every field below
// is `lock`'s. The clipboard's rule applies (clipboard.c): allocate and free
// OUTSIDE the lock, swap pointers UNDER it, because kmalloc takes the
// allocator's own interrupts-off lock.
struct console_font_pending
{
	spinlock_t lock;
	uint8_t *bytes;
	size_t length, cap;
	bool poisoned;
};

// Guards the queue slot, the active pointer and the verdict text. irqsave:
// the door runs with interrupts off and the status reader may not.
static spinlock_t s_lock = 0;
static console_loaded_t *s_queued;      // accepted, waiting for kworker
static bool s_queued_boot;              // ...or a request for the boot face
static volatile bool s_pending;         // either of the two above
static console_loaded_t *s_active;      // NULL while the boot face is in use
static char s_last[160] = "nothing loaded since boot";
// `last:` IS ABOUT THE MOST RECENT OFFER, so every judged offer takes a
// number, and the swap — which finishes long after the close that queued it —
// speaks only while its offer is still the newest. A font refused in the
// meantime has no swap coming to say so again; its reason must not be
// written over by news of the font before it.
static uint64_t s_offers;               // offers judged so far
static uint64_t s_queued_offer;         // the one in the slot

static void loaded_free(console_loaded_t *f)
{
	if (f == NULL)
		return;
	if (f->image != NULL)    kfree(f->image);
	if (f->resolved != NULL) kfree((void *)f->resolved);
	if (f->extra != NULL)    kfree(f->extra);
	kfree(f);
}

// An offer judged and NOT queued — a refusal. (An accepted one is numbered
// and announced by queue(), with the slot, in one critical section.)
static void verdict(const char *fmt, ...)
{
	char line[sizeof(s_last)];
	va_list args;
	va_start(args, fmt);
	vsnprintf(line, sizeof(line), fmt, args);
	va_end(args);

	uint64_t flags = spinlock_acquire_irqsave(&s_lock);
	s_offers++;
	memcpy(s_last, line, sizeof(s_last));
	spinlock_release_irqrestore(&s_lock, flags);
	printd(DEBUG_SYSTEM, "console font: %s\n", line);
}

// What became of offer number `offer`, from the swap. The log hears it
// either way; the status line only if no offer has been judged since.
static void sweep_verdict(uint64_t offer, const char *fmt, ...)
{
	char line[sizeof(s_last)];
	va_list args;
	va_start(args, fmt);
	vsnprintf(line, sizeof(line), fmt, args);
	va_end(args);

	uint64_t flags = spinlock_acquire_irqsave(&s_lock);
	if (s_offers == offer)
		memcpy(s_last, line, sizeof(s_last));
	spinlock_release_irqrestore(&s_lock, flags);
	printd(DEBUG_SYSTEM, "console font: %s\n", line);
}

// ── The door ────────────────────────────────────────────────────────────────

console_font_pending_t *console_font_begin(void)
{
	if (!kTTYReady)
		return NULL;
	return kmalloc(sizeof(console_font_pending_t));   // zeroed: empty, unpoisoned
}

int console_font_append(console_font_pending_t *p, const void *bytes, size_t length)
{
	if (p == NULL || bytes == NULL)
		return -1;

	for (;;)
	{
		uint64_t flags = spinlock_acquire_irqsave(&p->lock);
		// One byte of headroom past the fence, so "exactly the largest image"
		// and "too large" are different lengths by the time the loader is asked.
		if (p->poisoned || length > (size_t)PSF2_IMAGE_MAX + 1 - p->length)
		{
			p->poisoned = true;
			spinlock_release_irqrestore(&p->lock, flags);
			return -1;
		}
		size_t need = p->length + length;
		if (need <= p->cap)
		{
			memcpy(p->bytes + p->length, bytes, length);
			p->length = need;
			spinlock_release_irqrestore(&p->lock, flags);
			return 0;
		}
		spinlock_release_irqrestore(&p->lock, flags);

		// Grow with no lock held, then offer the larger buffer. Another
		// writer may have grown it meanwhile — then ours is the spare — or
		// appended enough that ours is already too small, and the loop asks
		// again. The fence bounds `need`, so the doubling stops.
		size_t cap = 65536;
		while (cap < need)
			cap *= 2;
		uint8_t *grown = kmalloc(cap);

		flags = spinlock_acquire_irqsave(&p->lock);
		uint8_t *spare = grown;
		if (p->cap < cap)
		{
			if (p->length > 0)
				memcpy(grown, p->bytes, p->length);
			spare = p->bytes;
			p->bytes = grown;
			p->cap = cap;
		}
		spinlock_release_irqrestore(&p->lock, flags);
		if (spare != NULL)
			kfree(spare);
	}
}

void console_font_discard(console_font_pending_t *p)
{
	if (p == NULL)
		return;
	if (p->bytes != NULL)
		kfree(p->bytes);
	kfree(p);
}

// Is the whole of what was written the word "boot"?
static bool is_boot_word(const uint8_t *b, size_t n)
{
	while (n > 0 && (b[n - 1] == ' ' || b[n - 1] == '\t' || b[n - 1] == '\n' || b[n - 1] == '\r'))
		n--;
	while (n > 0 && (*b == ' ' || *b == '\t' || *b == '\n' || *b == '\r'))
	{
		b++;
		n--;
	}
	return n == 4 && b[0] == 'b' && b[1] == 'o' && b[2] == 'o' && b[3] == 't';
}

// Everything the blitter will need, decided once. After this no draw asks
// whether the face has a glyph: it has SOMETHING for every byte of both sets.
static void resolve(console_loaded_t *f, const psf2_face_t *psf, const psf2_charmap_t *map)
{
	for (uint32_t i = 0; i < SYNTH_COUNT; i++)
		psf2_synth_block(kSynthBlocks[i], psf->width, psf->height,
		                 f->extra + (size_t)(i + 1) * psf->glyph_bytes);

	for (uint32_t set = 0; set < 2; set++)
	{
		for (uint32_t b = 0; b < 256; b++)
		{
			const uint8_t *bitmap = f->extra;   // blank
			uint16_t g = map->glyph[set][b];
			if (g != PSF2_MAP_NONE)
				bitmap = psf->glyphs + (size_t)g * psf->glyph_bytes;
			else
			{
				uint32_t cp = (set == OS64_CHARSET_CP437) ? os64_cp437_codepoint((uint8_t)b) : b;
				for (uint32_t i = 0; i < SYNTH_COUNT; i++)
					if (kSynthBlocks[i] == cp)
						bitmap = f->extra + (size_t)(i + 1) * psf->glyph_bytes;
			}
			f->resolved[set * 256 + b] = bitmap;
		}
	}
}

// The slot and the words about it change in ONE critical section: two closes
// can race here, and whichever wins the slot must also be the one `last:`
// describes, or a program polling the status credits the pending install to
// the offer that lost.
static void queue(console_loaded_t *f, bool boot, const char *fmt, ...)
{
	char line[sizeof(s_last)];
	va_list args;
	va_start(args, fmt);
	vsnprintf(line, sizeof(line), fmt, args);
	va_end(args);

	uint64_t flags = spinlock_acquire_irqsave(&s_lock);
	console_loaded_t *superseded = s_queued;   // the last word wins
	s_queued = f;
	s_queued_boot = boot;
	s_queued_offer = ++s_offers;
	s_pending = true;
	memcpy(s_last, line, sizeof(s_last));
	spinlock_release_irqrestore(&s_lock, flags);
	loaded_free(superseded);
	printd(DEBUG_SYSTEM, "console font: %s\n", line);
}

int console_font_submit(console_font_pending_t *p)
{
	if (p == NULL)
		return -1;

	// Close the image to further writing before judging it: whatever is
	// judged below is what gets installed, so it must not be able to change
	// underneath the judgement. After this an append is refused.
	uint64_t pflags = spinlock_acquire_irqsave(&p->lock);
	bool poisoned = p->poisoned;
	p->poisoned = true;
	spinlock_release_irqrestore(&p->lock, pflags);

	if (poisoned || p->length == 0)
	{
		verdict(poisoned ? "refused: image larger than %u bytes" : "refused: nothing was written",
		        (unsigned)PSF2_IMAGE_MAX);
		console_font_discard(p);
		return -1;
	}
	if (is_boot_word(p->bytes, p->length))
	{
		console_font_discard(p);
		queue(NULL, true, "accepted: the boot face, waiting for the swap");
		return 0;
	}

	psf2_face_t psf;
	psf2_charmap_t map;
	uint32_t why = 0;
	psf2_status_t st = psf2_parse(p->bytes, p->length, &psf, &why);
	if (st == PSF2_OK)
		st = psf2_build_charmap(&psf, &map, &why);
	if (st != PSF2_OK)
	{
		verdict("refused: %s (%u)", psf2_status_name(st), why);
		console_font_discard(p);
		return -1;
	}

	uint32_t cols = kFrameBuffer.width / psf.width;
	uint32_t rows = kFrameBuffer.height / psf.height;
	uint32_t cols_max, rows_max;
	tty_refont_limits(&cols_max, &rows_max);
	if (cols < CONSOLE_GRID_COLS_MIN || rows < CONSOLE_GRID_ROWS_MIN ||
	    cols > cols_max || rows > rows_max)
	{
		verdict("refused: a %ux%u cell makes this screen %ux%u, outside %ux%u..%ux%u",
		        psf.width, psf.height, cols, rows,
		        CONSOLE_GRID_COLS_MIN, CONSOLE_GRID_ROWS_MIN, cols_max, rows_max);
		console_font_discard(p);
		return -1;
	}

	console_loaded_t *f = kmalloc(sizeof(*f));
	f->image    = p->bytes;        // the pending's buffer becomes the face's
	f->resolved = kmalloc(2 * 256 * sizeof(const uint8_t *));
	f->extra    = kmalloc((size_t)(SYNTH_COUNT + 1) * psf.glyph_bytes);
	f->cols = cols;
	f->rows = rows;
	f->from_table  = map.from_table;
	f->unmapped[0] = map.unmapped[0];
	f->unmapped[1] = map.unmapped[1];
	resolve(f, &psf, &map);
	f->face = (console_face_t){
		.glyphs = psf.glyphs, .nglyphs = psf.nglyphs,
		.width = psf.width, .height = psf.height,
		.row_bytes = psf.row_bytes, .glyph_bytes = psf.glyph_bytes,
		.resolved = f->resolved,
	};
	p->bytes = NULL;               // ...so discarding the pending must not free it
	console_font_discard(p);

	queue(f, false, "accepted: %ux%u cell, %ux%u grid, waiting for the swap",
	      psf.width, psf.height, cols, rows);
	return 0;
}

bool console_font_pending(void)
{
	return s_pending;
}

// ── The swap (kworker) ──────────────────────────────────────────────────────

bool console_font_sweep(void)
{
	if (!s_pending)
		return false;

	uint64_t flags = spinlock_acquire_irqsave(&s_lock);
	console_loaded_t *next = s_queued;
	bool boot = s_queued_boot;
	uint64_t offer = s_queued_offer;
	s_queued = NULL;
	s_queued_boot = false;
	s_pending = false;
	spinlock_release_irqrestore(&s_lock, flags);
	if (next == NULL && !boot)
		return false;

	uint32_t cols, rows;
	if (next != NULL)
	{
		cols = next->cols;
		rows = next->rows;
	}
	else
	{
		const console_face_t *bf = renderer_boot_face();
		cols = kFrameBuffer.width / bf->width;
		rows = kFrameBuffer.height / bf->height;
	}

	// THE GRIDS FIRST, THEN THE FACE, THEN THE PICTURE. Only the focused
	// terminal paints, so it goes last and the interval in which its cells
	// and the face disagree is as short as it can be made; in that interval
	// a write lands new-grid cells at old-face positions, which is ugly and
	// in bounds (every glyph write clips to the framebuffer), and the repaint
	// below is what ends it. Focus can move meanwhile — the repaint asks who
	// has the glass at that moment rather than remembering.
	//
	// ALL EIGHT OR NONE. A terminal that will not take the new shape while
	// the face goes in anyway would wrap, repaint and report its size by one
	// cell and be painted with another — so a refusal puts back the ones
	// already reshaped and installs nothing. tty_refont does not give up on
	// a busy terminal (tty.c); what it still refuses is a terminal whose
	// own state fails the reflow's checks, which is a bug to be told about,
	// not a font to be loaded on top of.
	uint32_t dropped = 0, clipped = 0, reshaped = 0;
	uint32_t was_cols[TTY_COUNT], was_rows[TTY_COUNT];
	bool changed[TTY_COUNT] = { false };
	int refused = -1;
	tty_t *focused = kTTYFocused;
	for (uint32_t pass = 0; pass < 2 && refused < 0; pass++)
	{
		for (uint32_t i = 0; i < TTY_COUNT; i++)
		{
			tty_t *t = &kTTY[i];
			if ((t == focused) != (pass == 1))
				continue;
			uint32_t d = 0, c = 0;
			was_cols[i] = t->cols;
			was_rows[i] = t->rows;
			int r = tty_refont(t, cols, rows, &d, &c);
			if (r < 0)
			{
				refused = (int)i;
				break;
			}
			changed[i] = r > 0;
			reshaped += r > 0;
			dropped += d;
			clipped += c;
		}
	}
	if (refused >= 0)
	{
		for (uint32_t i = 0; i < TTY_COUNT; i++)
			if (changed[i])
				(void)tty_refont(&kTTY[i], was_cols[i], was_rows[i], NULL, NULL);
		tty_repaint_focused();
		loaded_free(next);
		sweep_verdict(offer, "refused: tty%u would not take a %ux%u grid; nothing was changed",
		              (unsigned)refused + 1, cols, rows);
		return true;
	}

	// Refused only by a panic that has the glass. The machine is going down,
	// and the renderer may still be reading the face this would retire, so
	// both faces are left exactly where they are: nothing is freed, and
	// nothing is reported about a swap that did not happen.
	if (!renderer_face_install(next != NULL ? &next->face : NULL))
		return true;
	tty_repaint_focused();

	// Every glyph read happens under the renderer lock, and the install has
	// returned: nothing can still be looking at the outgoing face.
	flags = spinlock_acquire_irqsave(&s_lock);
	console_loaded_t *outgoing = s_active;
	s_active = next;
	spinlock_release_irqrestore(&s_lock, flags);
	loaded_free(outgoing);

	// The window just changed size under every program at a VT. WHICH
	// terminal a task is at is task_tty's answer, not the raw field's: a task
	// with no terminal of record is at VT1 — that is what it reads its
	// geometry from and what /proc reports — so skipping it for having a
	// NULL there leaves it laying out for a shape that is gone.
	//
	// And only when it DID change size: a face with the cell of the one it
	// replaces reshapes nothing, and SIGWINCH says "your window is a
	// different size now", which would be a lie a program redraws for.
	uint32_t told = 0;
	for (task_t *seat = kTaskList;
	     reshaped > 0 && seat != NULL && seat != (task_t *)NO_TASK; seat = seat->next)
	{
		if (seat == kKernelTask || seat->exited)
			continue;
		tty_t *tt = task_tty(seat);
		if (tt < &kTTY[0] || tt >= &kTTY[TTY_COUNT])
			continue;
		if (task_signal_and_nudge(seat, SIGWINCH))
			told++;
	}

	sweep_verdict(offer, "installed: %s, %ux%u grid, %u terminals reshaped, %u told; "
	              "%u history lines dropped, %u rows clipped",
	              next != NULL ? "loaded face" : "boot face", cols, rows, reshaped, told,
	              dropped, clipped);
	return true;
}

// ── /sys/console/font ───────────────────────────────────────────────────────

size_t console_font_status(char *out, size_t cap)
{
	if (out == NULL || cap == 0)
		return 0;

	uint64_t flags = spinlock_acquire_irqsave(&s_lock);
	const console_face_t *bf = renderer_boot_face();
	const console_face_t *face = (s_active != NULL) ? &s_active->face : bf;
	int n = snprintf(out, cap, "source: %s\ncell: %ux%u\nglyphs: %u\n",
	                 s_active != NULL ? "loaded" : "boot",
	                 face->width, face->height, face->nglyphs);
	if (n > 0 && (size_t)n < cap && s_active != NULL)
		n += snprintf(out + n, cap - (size_t)n, "table: %s\nunmapped: latin1 %u cp437 %u\n",
		              s_active->from_table ? "yes" : "no",
		              s_active->unmapped[0], s_active->unmapped[1]);
	if (n > 0 && (size_t)n < cap)
		// The screen the grid was computed against, in pixels: a program
		// choosing a size for a grid it wants needs the dividend, not a
		// quotient it would have to invert to within a column.
		n += snprintf(out + n, cap - (size_t)n, "screen: %ux%u\ngrid: %ux%u\npending: %s\nlast: %s\n",
		              (unsigned)kFrameBuffer.width, (unsigned)kFrameBuffer.height,
		              kFrameBuffer.width / face->width, kFrameBuffer.height / face->height,
		              s_pending ? "yes" : "no", s_last);
	spinlock_release_irqrestore(&s_lock, flags);

	if (n < 0)
		return 0;
	return (size_t)n < cap ? (size_t)n : cap - 1;
}
