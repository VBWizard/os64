#include "os64/os64.h"
#include "image/image.h"
#include "image/sequence.h"
#include "vectors.h"
#include "sequence_vectors.h"

static bool check(const uint8_t *data, size_t size, const uint8_t *expected)
{
    os64_image_t image;
    if (os64_image_decode(data, size, &image) != OS64_IMAGE_OK)
        return false;
    uint32_t w = expected[0] | ((uint32_t)expected[1] << 8);
    uint32_t h = expected[4] | ((uint32_t)expected[5] << 8);
    bool good = image.width == w && image.height == h;
    const uint8_t *pixels = (const uint8_t *)image.pixels;
    for (size_t i = 0; good && i < (size_t)w*h*4; i++)
        good = pixels[i] == expected[i+8];
    os64_image_free(&image);
    return good && !image.pixels && !image.width && !image.height;
}

static uint32_t read32(const uint8_t *p)
{
    return p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}

static bool check_sequence(const uint8_t *data, size_t len, const uint8_t *expected)
{
    os64_image_sequence_t *seq;
    if (os64_image_sequence_decode(data,len,&seq) != OS64_IMAGE_OK) return false;
    uint32_t width=read32(expected+4), height=read32(expected+8);
    uint32_t count=read32(expected+12), plays=read32(expected+16);
    unsigned passes=plays ? plays : 2;
    bool good=true;
    for (unsigned pass=0;good && pass<passes;pass++) {
        const uint8_t *p=expected+20;
        for (unsigned i=0;good && i<count;i++) {
            const os64_image_frame_t *f=os64_image_sequence_frame(seq);
            good=f->width==width && f->height==height && f->frame_count==count &&
                 f->play_count==plays && f->index==i && f->delay_ms==read32(p);
            p+=4;
            const uint8_t *px=(const uint8_t *)f->pixels;
            for (size_t n=0;good && n<(size_t)width*height*4;n++) good=px[n]==p[n];
            p+=(size_t)width*height*4;
            os64_image_status_t want=(plays && pass+1==plays && i+1==count) ?
                                    OS64_IMAGE_END : OS64_IMAGE_OK;
            if (good) good=os64_image_sequence_next(seq)==want;
        }
    }
    if (good) good=os64_image_sequence_rewind(seq)==OS64_IMAGE_OK &&
                   os64_image_sequence_frame(seq)->index==0;
    os64_image_sequence_free(seq);
    return good;
}

int main(void)
{
    if (!check(gif_data_0, sizeof gif_data_0, gif_pixels_0) ||
        !check(gif_data_1, sizeof gif_data_1, gif_pixels_1) ||
        !check(gif_data_2, sizeof gif_data_2, gif_pixels_2) ||
        !check(gif_data_3, sizeof gif_data_3, gif_pixels_3) ||
        !check(gif_data_4, sizeof gif_data_4, gif_pixels_4))
        return 1;
    for (size_t n = 6; n < sizeof gif_data_0; n++) {
        os64_image_t image;
        if (os64_image_decode(gif_data_0, n, &image) != OS64_IMAGE_MALFORMED ||
            image.pixels || image.width || image.height)
            return 2;
    }
    char path[96];
    os64_snprintf(path, sizeof path, "/tmp/giftest-%lu.gif", os64_taskid());
    int64_t handle = os64_open(path, "w");
    if (handle < 0)
        return 3;
    int64_t written = os64_write((int32_t)handle, gif_data_0, sizeof gif_data_0);
    int64_t closed = os64_close((int32_t)handle);
    os64_image_t loaded;
    os64_image_status_t status = os64_image_load(path, 0, &loaded);
    bool good = written == sizeof gif_data_0 && closed == 0 && status == OS64_IMAGE_OK &&
                loaded.width == 19 && loaded.height == 11;
    os64_image_free(&loaded);
    os64_image_sequence_t *seq;
    if (os64_image_sequence_load(path,0,&seq)!=OS64_IMAGE_OK) good=false;
    else {
        good=good && os64_image_sequence_frame(seq)->frame_count==1 &&
             os64_image_sequence_next(seq)==OS64_IMAGE_END;
        os64_image_sequence_free(seq);
    }
    if (os64_unlink(path) < 0 || !good || os64_heap_verify())
        return 4;
    if (!check_sequence(sequence_data_0,sizeof sequence_data_0,sequence_expected_0) ||
        !check_sequence(sequence_data_1,sizeof sequence_data_1,sequence_expected_1) ||
        !check_sequence(sequence_data_2,sizeof sequence_data_2,sequence_expected_2) ||
        os64_heap_verify()) return 5;
    os64_printf("PASS GIF first-frame decode, animation canvases, disposal, loops, rewind, file loading and heap cleanup\n");
    return 0;
}
