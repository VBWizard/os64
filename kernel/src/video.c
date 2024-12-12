#include <stdbool.h>
#include "CONFIG.h"
#include "video.h"
#include "strings/sprintf.h"
#include "memset.h"
#include "limine_os64.h"

extern volatile bool kFBInitDone;

BasicRenderer kRenderer;
struct limine_framebuffer *kLimineFrameBuffer;
struct Framebuffer kFrameBuffer;
struct PSF1_FONT tFont;
struct PSF1_FONT* font;

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

    clear(&kRenderer, 0xff000080, true); // color blue
	moveto(&kRenderer, 0, 0);
	kFBInitDone = true;
}
