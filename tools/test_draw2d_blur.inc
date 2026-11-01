#include <draw2d/blur.h>

/* A direct convolution oracle: no sliding sum or in-place row workspace. */
static void blur_oracle(uint16_t *pixels, unsigned w, unsigned h, unsigned stride,
                        unsigned radius, uint16_t edge)
{
    uint16_t out[100];
    for(unsigned pass=0;pass<6;pass++) {
        for(unsigned y=0;y<h;y++)for(unsigned x=0;x<w;x++) {
            uint64_t sum=0;
            for(int d=-(int)radius;d<=(int)radius;d++) {
                int xx=(int)x+(pass%2?0:d),yy=(int)y+(pass%2?d:0);
                sum+=xx<0||yy<0||xx>=(int)w||yy>=(int)h?edge:pixels[yy*stride+xx];
            }
            out[y*w+x]=(uint16_t)((sum+radius)/(2*radius+1));
        }
        for(unsigned y=0;y<h;y++)for(unsigned x=0;x<w;x++)pixels[y*stride+x]=out[y*w+x];
    }
}
static void shared_blur(void)
{
    const uint16_t edges[]={0,256,65535};
    for(unsigned w=1;w<=7;w++)for(unsigned h=1;h<=7;h++)
    for(unsigned radius=1;radius<=9;radius++)for(unsigned e=0;e<3;e++) {
        unsigned stride=w+2;
        size_t entries=(h-1)*stride+w;
        uint16_t *pixels=malloc(entries*sizeof(*pixels)), want[100],scratch[7];
        CHECK(pixels!=NULL);
        for(size_t i=0;i<entries;i++)pixels[i]=want[i]=(uint16_t)((i*997+w*17+h*31)%65536);
        draw2d_mask16_t mask={pixels,w,h,stride*sizeof(uint16_t),entries*sizeof(uint16_t)};
        blur_oracle(want,w,h,stride,radius,edges[e]);
        CHECK(draw2d_blur_mask16(&mask,radius,edges[e],scratch,sizeof(scratch),(size_t)w*h)==DRAW2D_OK);
        CHECK(!memcmp(pixels,want,entries*sizeof(uint16_t)));
        free(pixels);
    }
    uint16_t data[12]={0,0,0,0,65535,0,0,0,0,0,0,0}, saved[12],scratch[3];
    draw2d_mask16_t mask={data,3,3,8,22};
    memcpy(saved,data,sizeof(data));
    CHECK(draw2d_blur_mask16(&mask,0,0,NULL,0,9)==DRAW2D_OK);
    CHECK(draw2d_blur_mask16(&mask,1,0,scratch,sizeof(scratch),8)==DRAW2D_LIMIT);
    CHECK(draw2d_blur_mask16(&mask,1,0,scratch,5,9)==DRAW2D_LIMIT);
    CHECK(draw2d_blur_mask16(&mask,1,0,data+2,6,9)==DRAW2D_BAD_ARGUMENT);
    CHECK(draw2d_blur_mask16(&mask,1,0,NULL,6,9)==DRAW2D_BAD_ARGUMENT);
    CHECK(!memcmp(data,saved,sizeof(data)));
    draw2d_mask16_t bad=mask;bad.capacity=21;
    CHECK(draw2d_blur_mask16(&bad,1,0,scratch,6,9)==DRAW2D_BAD_ARGUMENT);
    bad=mask;bad.stride=5;
    CHECK(draw2d_blur_mask16(&bad,1,0,scratch,6,9)==DRAW2D_BAD_ARGUMENT);
    bad=mask;bad.stride=4;
    CHECK(draw2d_blur_mask16(&bad,1,0,scratch,6,9)==DRAW2D_BAD_ARGUMENT);
    bad=mask;bad.pixels=NULL;
    CHECK(draw2d_blur_mask16(&bad,0,0,NULL,0,9)==DRAW2D_BAD_ARGUMENT);
    bad=mask;bad.pixels=(uint16_t *)((uint8_t *)data+1);
    CHECK(draw2d_blur_mask16(&bad,1,0,scratch,6,9)==DRAW2D_BAD_ARGUMENT);
    CHECK(!memcmp(data,saved,sizeof(data)));
    bad=(draw2d_mask16_t){0};
    CHECK(draw2d_blur_mask16(&bad,UINT32_MAX,65535,NULL,0,0)==DRAW2D_OK);
    CHECK(draw2d_blur_mask16(NULL,1,0,scratch,6,9)==DRAW2D_BAD_ARGUMENT);
    // A very large kernel is still bounded by mask area, with a 64-bit sum.
    data[0]=12345;mask=(draw2d_mask16_t){data,1,1,2,2};
    CHECK(draw2d_blur_mask16(&mask,UINT32_MAX,65535,scratch,2,1)==DRAW2D_OK);
    CHECK(data[0]==65535);
    CHECK(draw2d_blur_mask16(&mask,UINT32_MAX,0,scratch,2,1)==DRAW2D_OK);
    CHECK(data[0]==0);
}
