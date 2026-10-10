#include "os64/os64.h"
#include "os64/mem.h"
#include "os64/str.h"
#include "image/image.h"
#include "image/sequence.h"
#include "webp/webp.h"
#include "vectors.h"

static unsigned start;
static uint32_t le32(const uint8_t *p)
{ return p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
static bool check(const uint8_t *p, size_t n, const uint8_t *ref)
{
    os64_image_t image;
    if (os64_image_decode(p,n,&image) != OS64_IMAGE_OK) return false;
    bool ok = image.width == le32(ref) && image.height == le32(ref+4) &&
        !os64_memcmp(image.pixels,ref+8,(size_t)image.width*image.height*4);
    os64_image_free(&image);
    return ok && !image.pixels && !image.width && !image.height;
}
static int64_t worker(void *arg)
{
    uintptr_t which = (uintptr_t)arg % 3;
    while (!__atomic_load_n(&start,__ATOMIC_ACQUIRE)) os64_sleep(1);
    const uint8_t *p = which == 0 ? lossy_webp : which == 1 ? alpha_webp : lossless_webp;
    size_t n = which == 0 ? sizeof lossy_webp : which == 1 ? sizeof alpha_webp : sizeof lossless_webp;
    const uint8_t *ref = which == 0 ? lossy_pixels : which == 1 ? alpha_pixels : lossless_pixels;
    for (unsigned i=0;i<10;i++) {
        if (!check(p,n,ref)) return 1;
        os64_webp_image_t im;
        if (os64_webp_decode(p,n,0,1,&im) != OS64_WEBP_LIMIT || im.pixels) return 2;
    }
    return 0;
}
int main(void)
{
    int64_t threads[6], answer;
    for (uintptr_t i=0;i<6;i++) {
        threads[i]=os64_thread(worker,(void *)i);
        if (threads[i]<0) return 1;
    }
    __atomic_store_n(&start,1,__ATOMIC_RELEASE);
    for (unsigned i=0;i<6;i++) {
        if (os64_thread_join((int32_t)threads[i],&answer)<0 || answer) return 2;
        os64_close((int32_t)threads[i]);
    }
    os64_webp_image_t image;
    for (size_t n=0;n<sizeof alpha_webp;n++)
        if (os64_webp_decode(alpha_webp,n,0,0,&image)==OS64_WEBP_OK || image.pixels) return 3;
    os64_image_t still;
    if (os64_image_decode(animated_webp,sizeof animated_webp,&still)!=OS64_IMAGE_UNSUPPORTED || still.pixels) return 4;
    os64_image_sequence_t *seq = NULL;
    if (os64_image_sequence_decode(lossless_webp,sizeof lossless_webp,&seq)!=OS64_IMAGE_OK) return 5;
    const os64_image_frame_t *frame=os64_image_sequence_frame(seq);
    bool ok=frame && frame->frame_count==1 && frame->width==31 && frame->height==19 &&
        !os64_memcmp(frame->pixels,lossless_pixels+8,31*19*4) && os64_image_sequence_next(seq)==OS64_IMAGE_END;
    os64_image_sequence_free(seq);
    if (!ok) return 6;
    char path[96]; os64_snprintf(path,sizeof path,"/home/webptest-%lu.jpg",os64_taskid());
    int64_t fd=os64_open(path,"w");
    if (fd<0) return 7;
    int64_t written=os64_write((int32_t)fd,alpha_webp,sizeof alpha_webp);
    os64_close((int32_t)fd);
    os64_image_status_t status=os64_image_load(path,0,&still);
    ok=written==sizeof alpha_webp && status==OS64_IMAGE_OK && still.width==31 && still.height==19 &&
        !os64_memcmp(still.pixels,alpha_pixels+8,31*19*4);
    os64_image_free(&still);
    if (os64_unlink(path)<0 || !ok || os64_heap_verify()) return 8;
    os64_printf("PASS WebP lossy/lossless/alpha pixels, six callers, limits, truncation, animation refusal, sequence, mislabeled file and heap\n");
    return 0;
}
