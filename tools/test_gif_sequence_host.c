// Incremental sequence oracle: complete frames, loops, ownership and failure atomicity.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stddef.h>
#include <stdbool.h>
#include "image/sequence.h"
#include "png/png.h"
#include "jpeg/jpeg.h"
#include "os64/slurp.h"

typedef union { size_t n; max_align_t align; } allocation;
static size_t live, bytes, peak, attempts, fail_at;
void *os64_malloc(size_t n)
{
    attempts++;
    if (fail_at && attempts == fail_at) return NULL;
    allocation *h = malloc(sizeof(*h) + n); assert(h);
    h->n = n; live++; bytes += n;
    if (bytes > peak) peak = bytes;
    return h + 1;
}
void os64_free(void *p)
{
    if (!p) return;
    allocation *h = (allocation *)p - 1;
    assert(live && bytes >= h->n); live--; bytes -= h->n; free(h);
}
void *os64_memcpy(void *d, const void *s, size_t n) { return memcpy(d,s,n); }
void *os64_memset(void *d, int c, size_t n) { return memset(d,c,n); }
int os64_memcmp(const void *a, const void *b, size_t n) { return memcmp(a,b,n); }
os64_png_status_t os64_png_decode(const uint8_t *p, size_t n, uint64_t c, os64_png_image_t *o)
{ (void)p; (void)n; (void)c; (void)o; abort(); }
os64_jpeg_status_t os64_jpeg_decode(const uint8_t *p, size_t n, uint64_t c, size_t m, os64_jpeg_image_t *o)
{ (void)p; (void)n; (void)c; (void)m; (void)o; abort(); }
os64_slurp_status_t os64_slurp(const char *p, size_t c, uint8_t **o, size_t *n)
{ (void)p; (void)c; (void)o; (void)n; abort(); }

static uint32_t word(FILE *f) { uint32_t n; assert(fread(&n,4,1,f)==1); return n; }
static void unchanged(const os64_image_frame_t *f, const uint32_t *pixels,
                       size_t n, uint32_t index)
{ assert(f->index == index && !memcmp(f->pixels,pixels,n)); }

int main(int argc, char **argv)
{
    assert(argc == 3 || argc == 4);
    FILE *in=fopen(argv[1],"rb"), *ref=fopen(argv[2],"rb"); assert(in && ref);
    assert(!fseek(in,0,SEEK_END)); long length=ftell(in); assert(length>0); rewind(in);
    size_t len=(size_t)length;
    uint8_t *data=malloc(len); assert(data && fread(data,1,len,in)==len); fclose(in);
    uint32_t expected_status=word(ref);
    os64_image_sequence_t *seq=(void *)1;
    os64_image_status_t st=os64_image_sequence_decode(data,len,&seq);
    assert(st == expected_status);
    if (st != OS64_IMAGE_OK) {
        assert(!seq && !live && !bytes);
        assert(!attempts); // These fixtures refuse in metadata preflight.
        fclose(ref); free(data); return 0;
    }
    size_t allocations=attempts;
    os64_image_sequence_free(seq); assert(!live && !bytes);
    for (size_t a=1;a<=allocations;a++) {
        attempts=0; fail_at=a; seq=(void *)1;
        assert(os64_image_sequence_decode(data,len,&seq)==OS64_IMAGE_NO_MEMORY);
        assert(!seq && !live && !bytes);
    }
    fail_at=0;
    bool is_gif=len>=6 && !memcmp(data,"GIF8",4);
    if (is_gif) {
        size_t step=len>100000 ? 16381 : 1;
        for (size_t n=0;n<len;n+=step) {
            seq=(void *)1;
            st=os64_image_sequence_decode(data,n,&seq);
            assert(st==(n<6 ? OS64_IMAGE_UNKNOWN_FORMAT : OS64_IMAGE_MALFORMED));
            assert(!seq && !live && !bytes);
        }
        uint32_t rng=0x640416;
        for (unsigned n=0;n<128;n++) {
            rng=rng*1664525u+1013904223u;
            size_t at=6+rng%(len-6);
            uint8_t saved=data[at]; data[at]^=(uint8_t)(1u<<((rng>>24)&7));
            st=os64_image_sequence_decode(data,len,&seq);
            if (st==OS64_IMAGE_OK) {
                size_t opened=attempts;
                for (unsigned k=0;k<8;k++) {
                    st=os64_image_sequence_next(seq);
                    if (st!=OS64_IMAGE_OK) {
                        assert(st==OS64_IMAGE_END || st==OS64_IMAGE_MALFORMED);
                        break;
                    }
                }
                assert(attempts==opened);
                os64_image_sequence_free(seq);
            } else {
                assert(!seq);
                assert(st==OS64_IMAGE_MALFORMED || st==OS64_IMAGE_UNSUPPORTED || st==OS64_IMAGE_LIMIT);
            }
            assert(!live && !bytes);
            data[at]=saved;
        }
    }
    attempts=0;
    assert(os64_image_sequence_decode(data,len,&seq)==OS64_IMAGE_OK);
    // The caller can destroy its input immediately after opening.
    memset(data,0,len); free(data);
    uint32_t w=word(ref),h=word(ref),count=word(ref),plays=word(ref);
    const os64_image_frame_t *f=os64_image_sequence_frame(seq);
    assert(f->width==w && f->height==h && f->frame_count==count && f->play_count==plays);
    size_t plane=(size_t)w*h*4;
    uint32_t *pixels=malloc(plane); assert(pixels);
    size_t before=attempts;
    size_t sequence_bytes=bytes;
    unsigned passes=plays ? plays : 2;
    if (count==1) passes=1;
    for (unsigned pass=0;pass<passes;pass++) {
        assert(!fseek(ref,20,SEEK_SET));
        for (unsigned i=0;i<count;i++) {
            uint32_t delay=word(ref);
            assert(fread(pixels,1,plane,ref)==plane);
            assert(f->delay_ms==delay);
            if (f->index != i || memcmp(f->pixels,pixels,plane)) {
                fprintf(stderr,"%s pass %u frame %u mismatch\n",argv[1],pass,i);
                abort();
            }
            if (argc==4) {
                assert(os64_image_sequence_next(seq)==OS64_IMAGE_MALFORMED);
                unchanged(f,pixels,plane,i);
                assert(os64_image_sequence_next(seq)==OS64_IMAGE_MALFORMED);
                unchanged(f,pixels,plane,i);
                goto rewound;
            }
            st=os64_image_sequence_next(seq);
            if (count==1 || (plays && pass+1==plays && i+1==count)) {
                assert(st==OS64_IMAGE_END);
                unchanged(f,pixels,plane,i);
                assert(os64_image_sequence_next(seq)==OS64_IMAGE_END);
                unchanged(f,pixels,plane,i);
            } else assert(st==OS64_IMAGE_OK);
        }
    }
rewound:
    assert(os64_image_sequence_rewind(seq)==OS64_IMAGE_OK);
    assert(!fseek(ref,20,SEEK_SET)); assert(f->delay_ms==word(ref));
    assert(fread(pixels,1,plane,ref)==plane); unchanged(f,pixels,plane,0);
    assert(attempts==before); // Playback and rewind allocate nothing.
    os64_image_sequence_free(seq); assert(!live && !bytes);
    assert(peak <= 128u*1024u*1024u);
    printf("PASS %s: %u frames, %u plays, owned %zu bytes (mutation peak %zu)\n",argv[1],count,plays,sequence_bytes,peak);
    free(pixels); fclose(ref); return 0;
}
