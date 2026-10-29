/* Bounded sRGB RGBA8 PNG writer. Stored DEFLATE blocks avoid an encoder
 * workspace and make admission exact; callers own the output allocation. */
#include <png/png.h>
#include <os64/crc32.h>
#include <stdbool.h>

static void be32(uint8_t *p,uint32_t v)
{ p[0]=v>>24;p[1]=v>>16;p[2]=v>>8;p[3]=v; }
static uint8_t *chunk(uint8_t *p,const char *type,const uint8_t *data,size_t length)
{
    be32(p,(uint32_t)length);
    for(unsigned i=0;i<4;++i)p[4+i]=(uint8_t)type[i];
    for(size_t i=0;i<length;++i)p[8+i]=data[i];
    be32(p+8+length,os64_crc32(p+4,length+4));
    return p+length+12;
}
os64_png_status_t os64_png_encode_size(uint32_t width,uint32_t height,size_t *size)
{
    if(!size)return OS64_PNG_BAD_ARGUMENT;
    *size=0;
    if(!width||!height)return OS64_PNG_BAD_ARGUMENT;
    if(width>OS64_PNG_DIM_MAX||height>OS64_PNG_DIM_MAX||
       (uint64_t)width*height>OS64_PNG_PIXEL_CAP_DEFAULT)return OS64_PNG_LIMIT;
    uint64_t raw=((uint64_t)width*4+1)*height;
    uint64_t total=97+raw+5*((raw+65534)/65535);
    if(total>SIZE_MAX||total>UINT32_MAX)return OS64_PNG_LIMIT;
    *size=(size_t)total;return OS64_PNG_OK;
}
os64_png_status_t os64_png_encode(const uint32_t *pixels,uint32_t width,uint32_t height,
    size_t stride,size_t capacity,bool premultiplied,uint8_t *out,size_t out_capacity,size_t *written)
{
    if(!written)return OS64_PNG_BAD_ARGUMENT;
    *written=0;size_t needed;
    os64_png_status_t status=os64_png_encode_size(width,height,&needed);
    if(status!=OS64_PNG_OK)return status;
    size_t row=(size_t)width*4;
    if(!pixels||!out||(uintptr_t)pixels%_Alignof(uint32_t)||stride%_Alignof(uint32_t)||stride<row)
        return OS64_PNG_BAD_ARGUMENT;
    if((height>1&&stride>(SIZE_MAX-row)/(height-1))||
       (size_t)(height-1)*stride+row>capacity||out_capacity<needed)return OS64_PNG_LIMIT;
    uintptr_t a=(uintptr_t)pixels,b=(uintptr_t)out;
    if(capacity>UINTPTR_MAX-a||needed>UINTPTR_MAX-b)return OS64_PNG_LIMIT;
    if(a<b+needed&&b<a+capacity)return OS64_PNG_BAD_ARGUMENT;
    const uint8_t signature[]={137,80,78,71,13,10,26,10};
    for(unsigned i=0;i<8;++i)out[i]=signature[i];
    uint8_t ihdr[13]={0},physical[9]={0},intent=0;
    be32(ihdr,width);be32(ihdr+4,height);ihdr[8]=8;ihdr[9]=6;
    be32(physical,3780);be32(physical+4,3780);physical[8]=1;
    uint8_t *p=chunk(out+8,"IHDR",ihdr,13);
    p=chunk(p,"sRGB",&intent,1);p=chunk(p,"pHYs",physical,9);
    uint8_t *idat=p;size_t raw=(row+1)*height,compressed=6+raw+5*((raw+65534)/65535);
    be32(p,(uint32_t)compressed);p[4]='I';p[5]='D';p[6]='A';p[7]='T';p+=8;
    *p++=0x78;*p++=0x01;
    uint32_t adler_a=1,adler_b=0;
    size_t remaining=raw,position=0;
    while(remaining) {
        uint32_t n=remaining>65535?65535:(uint32_t)remaining;
        *p++=remaining==n?1:0;*p++=n;*p++=n>>8;*p++=~n;*p++=(~n)>>8;
        for(uint32_t i=0;i<n;++i,++position) {
            size_t column=position%(row+1),y=position/(row+1);uint8_t value=0;
            if(column) {
                size_t at=column-1;uint32_t argb=*(const uint32_t *)((const uint8_t *)pixels+y*stride+(at/4)*4);
                unsigned component=at%4,alpha=argb>>24;
                value=component==3?alpha:(argb>>(16-component*8))&255;
                if(premultiplied&&component<3) {
                    unsigned unassociated=alpha?((unsigned)value*255+alpha/2)/alpha:0;
                    value=unassociated>255?255:unassociated;
                }
            }
            *p++=value;adler_a=(adler_a+value)%65521;adler_b=(adler_b+adler_a)%65521;
        }
        remaining-=n;
    }
    be32(p,(adler_b<<16)|adler_a);p+=4;
    be32(p,os64_crc32(idat+4,compressed+4));p+=4;
    p=chunk(p,"IEND",NULL,0);*written=(size_t)(p-out);
    return OS64_PNG_OK;
}
