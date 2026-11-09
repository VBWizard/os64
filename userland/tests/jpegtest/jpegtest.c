#include "os64/os64.h"
#include "os64/mem.h"
#include "image/image.h"
#include "jpeg/jpeg.h"
#include "vectors.h"
#define JPEGTEST_OK 0x90650000
static bool check(const uint8_t *bytes, size_t size, const uint8_t *expected)
{
    os64_image_t image;
    if (os64_image_decode(bytes, size, &image) != OS64_IMAGE_OK) return false;
    uint32_t w = expected[0] | (uint32_t)expected[1]<<8;
    uint32_t h = expected[4] | (uint32_t)expected[5]<<8;
    bool good = image.width == w && image.height == h;
    const uint8_t *pixels = (const uint8_t *)image.pixels;
    for (size_t i=0;good && i<(size_t)w*h*4;i++) {
        int difference = (int)pixels[i] - expected[i+8];
        if (difference < -2 || difference > 2) good = false;
    }
    os64_image_free(&image);
    return good && !image.pixels && !image.width && !image.height;
}
int main(void)
{
    if (!check(oriented_jpeg,sizeof oriented_jpeg,oriented_pixels) ||
        !check(progressive_jpeg,sizeof progressive_jpeg,progressive_pixels)) return JPEGTEST_OK+1;
    os64_jpeg_image_t image;
    if (os64_jpeg_decode(oriented_jpeg,sizeof oriented_jpeg,10,0,&image)!=OS64_JPEG_LIMIT || image.pixels) return JPEGTEST_OK+2;
    for(size_t n=2;n<sizeof oriented_jpeg;n++) {
        if(os64_jpeg_decode(oriented_jpeg,n,0,0,&image)==OS64_JPEG_OK || image.pixels) return JPEGTEST_OK+3;
    }
    if (!check(oriented_jpeg,sizeof oriented_jpeg,oriented_pixels)) return JPEGTEST_OK+4;
    char path[96];os64_snprintf(path,sizeof path,"/tmp/jpegtest-%lu.jpg",os64_taskid());
    int64_t handle=os64_open(path,"w");
    if(handle<0) return JPEGTEST_OK+5;
    int64_t written=os64_write((int32_t)handle,oriented_jpeg,sizeof oriented_jpeg);
    int64_t closed=os64_close((int32_t)handle);
    os64_image_t loaded;
    os64_image_status_t status=os64_image_load(path,0,&loaded);
    bool good=written==sizeof oriented_jpeg && closed==0 && status==OS64_IMAGE_OK && loaded.width==9 && loaded.height==17;
    os64_image_free(&loaded);
    if(os64_unlink(path)<0 || !good || os64_heap_verify()) return JPEGTEST_OK+6;
    os64_serial_log("PASS JPEG baseline/progressive, EXIF rotation, shared image dispatch, truncation, limits, file loading and heap cleanup");
    return JPEGTEST_OK;
}
