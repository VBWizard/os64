#ifndef VIDEO_H
#define VIDEO_H

#include "stddef.h"
#include "limine.h"
#include "BasicRenderer.h"

struct Framebuffer
{
    void *base_address;
    size_t buffer_size;
    unsigned int width;
    unsigned int height;
    unsigned int pixels_per_scan_line;
};

struct PSF1_HEADER
{
    unsigned char magic[2];
    unsigned char mode;
    unsigned char charsize;
};

struct PSF1_FONT
{
    struct PSF1_HEADER *psf1_header;
    void *glyph_buffer;
};

extern struct limine_framebuffer *kLimineFrameBuffer;
extern struct Framebuffer kFrameBuffer;
void init_video(struct limine_framebuffer *fb, struct limine_module_response *module_response);

#endif 
