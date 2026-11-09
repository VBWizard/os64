#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <png/png.h>
void *os64_malloc(size_t n){return malloc(n);}
void os64_free(void *p){free(p);}
void *os64_memset(void *p,int c,size_t n){return memset(p,c,n);}
int main(int argc,char **argv)
{
    assert(argc==2);enum { W=257,H=257,STRIDE=260 };
    uint32_t *pixels=calloc(STRIDE*H,4);assert(pixels);
    for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W;++x) {
        unsigned a=(x+y)%256,r=x%256,g=y%256,b=(x^y)%256;
        pixels[y*STRIDE+x]=a<<24|r<<16|g<<8|b;
    }
    size_t needed,written;assert(os64_png_encode_size(W,H,&needed)==OS64_PNG_OK);
    uint8_t *bytes=malloc(needed+16);assert(bytes);memset(bytes,0x9a,needed+16);
    assert(os64_png_encode(pixels,W,H,STRIDE*4,STRIDE*H*4,false,bytes,needed-1,&written)==OS64_PNG_LIMIT&&written==0);
    for(size_t i=0;i<needed+16;++i)assert(bytes[i]==0x9a);
    assert(os64_png_encode(pixels,W,H,STRIDE*4,STRIDE*H*4,false,bytes,needed,&written)==OS64_PNG_OK&&written==needed);
    for(size_t i=needed;i<needed+16;++i)assert(bytes[i]==0x9a);
    FILE *f=fopen(argv[1],"wb");assert(f);assert(fwrite(bytes,1,written,f)==written);fclose(f);
    os64_png_image_t decoded;
    assert(os64_png_decode(bytes,written,0,&decoded)==OS64_PNG_OK);
    assert(decoded.width==W&&decoded.height==H);
    for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W;++x)assert(decoded.pixels[y*W+x]==pixels[y*STRIDE+x]);
    os64_png_free(&decoded);
    uint32_t premul[]={0x80402010,0x00000000};
    assert(os64_png_encode(premul,2,1,8,8,true,bytes,needed,&written)==OS64_PNG_OK);
    assert(os64_png_decode(bytes,written,0,&decoded)==OS64_PNG_OK);
    assert(decoded.pixels[0]==0x80804020&&decoded.pixels[1]==0);os64_png_free(&decoded);
    assert(os64_png_encode_size(0,1,&written)==OS64_PNG_BAD_ARGUMENT&&written==0);
    assert(os64_png_encode_size(UINT32_MAX,1,&written)==OS64_PNG_LIMIT&&written==0);
    free(bytes);free(pixels);puts("PNG encode: bounds, stored-block boundaries, alpha and round trips passed");return 0;
}
