#include <stdbool.h>
#include "CONFIG.h"
#include "video.h"
#include "strings/sprintf.h"
#include "memset.h"
#include "limine_os64.h"
#include "printd.h"
#include "os64/charset.h"   // the CP437 map this file checks back against the face

extern volatile bool kFBInitDone;

BasicRenderer kRenderer;
struct limine_framebuffer *kLimineFrameBuffer;
struct Framebuffer kFrameBuffer;
struct PSF1_FONT tFont;
struct PSF1_FONT* font;

// ── Is the CP437 map still true of this face? ───────────────────────────
//
// os64/charset.h names glyphs in the shipped face BY INDEX, because gterm's
// embedded font carries no Unicode table to look them up in and a map only
// one of the two painters could build is a map they would eventually
// disagree about. An index is only as stable as the face, though — so the
// face's OWN table is walked here and every entry checked back. A font that
// moved a glyph would otherwise change what a board's ANSI art looks like
// with nothing anywhere to point at.
//
// PSF1's table follows the bitmaps: per glyph, a run of little-endian
// uint16 code points ended by 0xFFFF, with 0xFFFE opening a multi-code-point
// sequence. Absent (mode bit 0x02 clear), there is nothing to check against
// and the map is taken on faith — which is the honest report, not a panic:
// the console works either way and only the high half is at stake.
static void charset_verify(struct PSF1_FONT *f, uint64_t bytes)
{
	uint32_t nglyphs = (f->psf1_header->mode & 0x01) ? 512 : 256;
	uint64_t used = sizeof(struct PSF1_HEADER)
	              + (uint64_t)nglyphs * f->psf1_header->charsize;

	if (!(f->psf1_header->mode & 0x02) || bytes <= used)
	{
		printd(DEBUG_BOOT, "charset: %s carries no unicode table - the CP437 map is unchecked\n",
		       FRAMEBUFFER_FONT);
		return;
	}

	uint16_t found[128];
	for (uint32_t i = 0; i < 128; i++)
		found[i] = OS64_CHARSET_NONE;

	const uint16_t *entry = (const uint16_t *)((const uint8_t *)f->psf1_header + used);
	uint64_t words = (bytes - used) / sizeof(uint16_t);
	uint32_t glyph = 0;

	for (uint64_t w = 0; w < words && glyph < nglyphs; w++)
	{
		uint16_t code = entry[w];
		if (code == 0xFFFF) { glyph++; continue; }
		if (code == 0xFFFE) continue;          // a sequence, not a single code point
		for (uint32_t i = 0; i < 128; i++)
			if (found[i] == OS64_CHARSET_NONE
			    && os64_cp437_codepoint((uint8_t)(0x80 + i)) == code)
				found[i] = (uint16_t)glyph;
	}

	uint32_t wrong = 0, absent = 0;
	for (uint32_t i = 0; i < 128; i++)
	{
		uint16_t claimed = os64_charset_entry((uint8_t)(0x80 + i));
		// A built-in bitmap claims nothing about the face, and the face is
		// free to grow the glyph later — that is not a drift.
		if (claimed >= OS64_CHARSET_BUILTIN && claimed != OS64_CHARSET_NONE)
			continue;
		if (claimed == OS64_CHARSET_NONE) { absent++; continue; }
		if (found[i] != claimed)
		{
			wrong++;
			if (found[i] == OS64_CHARSET_NONE)
				printd(DEBUG_BOOT, "charset: 0x%02X wants glyph %u, %s no longer carries it\n",
				       0x80 + i, claimed, FRAMEBUFFER_FONT);
			else
				printd(DEBUG_BOOT, "charset: 0x%02X wants glyph %u, %s holds it at %u\n",
				       0x80 + i, claimed, FRAMEBUFFER_FONT, found[i]);
		}
	}

	printd(DEBUG_BOOT, "charset: CP437 map checked against %s - %u drifted, %u not in the face\n",
	       FRAMEBUFFER_FONT, wrong, absent);
}

// Halt and catch fire function.
static void hcf(void) {
    for (;;) {
#if defined (__x86_64__)
        asm ("hlt");
#elif defined (__aarch64__) || defined (__riscv)
        asm ("wfi");
#elif defined (__loongarch64)
        asm ("idle 0");
#endif
    }
}

void init_video(struct limine_framebuffer *framebuffer, struct limine_module_response *module_response)
{
	font = &tFont;
	kLimineFrameBuffer = framebuffer;

	kFrameBuffer.base_address = framebuffer->address;
	kFrameBuffer.width = framebuffer->width;
	kFrameBuffer.height = framebuffer->height;
	kFrameBuffer.pixels_per_scan_line = framebuffer->pitch / 4;
	kFrameBuffer.buffer_size = framebuffer->height * framebuffer->pitch;

	const char *fName = FRAMEBUFFER_FONT;
	struct limine_file *file = getFile(module_response, fName);
	if (file == NULL)
	{
		hcf();
	}

	font->psf1_header = (struct PSF1_HEADER *)file->address;
	if (font->psf1_header->magic[0] != 0x36 || font->psf1_header->magic[1] != 0x04)
	{
		hcf();
	}

	font->glyph_buffer = (void *)((uint64_t)file->address + sizeof(struct PSF1_HEADER));
	init_renderer(&kRenderer, &kFrameBuffer, font);
	charset_verify(font, file->size);

    clear(&kRenderer, 0xff000080, true); // color blue
	moveto(&kRenderer, 0, 0);
	kFBInitDone = true;
}
