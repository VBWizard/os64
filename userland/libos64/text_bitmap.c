#include "text_internal.h"
#include "text_w1_data.h"
#include "os64/font_psf1.h"
#include "os64/charset.h"

const uint8_t *text_bitmap_rows(uint32_t c)
{
    if (c>=TEXT_CP437_BASE && c<TEXT_CP437_BASE+256)
        return os64_charset_glyph((uint8_t)(c-TEXT_CP437_BASE),OS64_CHARSET_CP437,os64_font_psf1+4,256,16);
    if (c<256) return os64_font_psf1+4+c*16;
    for (unsigned b=128;b<256;b++)
        if (os64_cp437_codepoint((uint8_t)b)==c) {
            if (os64_charset_entry((uint8_t)b)==OS64_CHARSET_NONE) return NULL;
            return os64_charset_glyph((uint8_t)b,OS64_CHARSET_CP437,os64_font_psf1+4,256,16);
        }
    return NULL;
}
static bool stroke(int32_t at, int32_t center, unsigned weight, int32_t thin, int32_t extent)
{
    /* Two rails collapse to one when the cell cannot contain their gap. */
    if (weight==3 && extent<4*thin) weight=1;
    int32_t d=at-center;
    if (weight==3) return (d>=-2*thin && d<-thin) || (d>=thin && d<2*thin);
    int32_t width=weight==2?2*thin:thin;
    return d>=-(width/2) && d<width-width/2;
}
uint8_t text_box_pixel(uint32_t c, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (c>=0x2580 && c<=0x259f) {
        bool ink=false;
        if (c==0x2580) ink=y<(h+1)/2;
        else if (c<=0x2588) ink=y>=h-((c-0x2580)*h+7)/8;
        else if (c<=0x258f) ink=x<((0x2590-c)*w+7)/8;
        else if (c==0x2590) ink=x>=w/2;
        else if (c<=0x2593) {
            static const uint8_t bayer[16]={0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5};
            ink=bayer[(y%4)*4+x%4]<4*(c-0x2590);
        } else if (c==0x2594) ink=y<(h+7)/8;
        else if (c==0x2595) ink=x>=w-(w+7)/8;
        else {
            /* Quadrant bits: upper-left, upper-right, lower-left, lower-right. */
            static const uint8_t quadrants[10]={4,8,1,13,9,7,11,2,6,14};
            unsigned bit=(y>=h/2?2:0)+(x>=w/2?1:0);
            ink=(quadrants[c-0x2596]&(1u<<bit))!=0;
        }
        return ink?255:0;
    }
    if (c<0x2500 || c>0x257f) return 0;
    size_t n=c-0x2500;
    int32_t cx=(int32_t)w/2,cy=(int32_t)h/2;
    int32_t thin=(int32_t)(w<h?w:h)/12;
    if (thin<1) thin=1;
    if (text_boxes[n].diagonal) {
        int64_t d1=(2*(int64_t)x+1)*h-(2*(int64_t)y+1)*w;
        int64_t d2=(2*(int64_t)x+1)*h-(2*(int64_t)(h-1-y)+1)*w;
        int64_t limit=(w>h?w:h)*thin;
        return (((text_boxes[n].diagonal&2) && d1>=-limit && d1<=limit) ||
            ((text_boxes[n].diagonal&1) && d2>=-limit && d2<=limit))?255:0;
    }
    if (text_boxes[n].arc && w>2 && h>2) {
        bool right=text_boxes[n].arms[1]!=0,down=text_boxes[n].arms[2]!=0;
        int32_t ex=right?(int32_t)w-1:0,ey=down?(int32_t)h-1:0;
        int64_t rx=right?ex-cx:cx,ry=down?ey-cy:cy;
        if ((right?(int32_t)x>=cx:(int32_t)x<=cx) &&
            (down?(int32_t)y>=cy:(int32_t)y<=cy)) {
            int64_t dx=(int64_t)x-ex,dy=(int64_t)y-ey;
            int64_t v=dx*dx*ry*ry+dy*dy*rx*rx;
            int64_t outer=rx*rx*ry*ry;
            int64_t irx=rx>thin?rx-thin:0,iry=ry>thin?ry-thin:0;
            return v<=outer && v>=irx*irx*iry*iry?255:0;
        }
        return 0;
    }
    bool ink=false;
    const uint8_t *a=text_boxes[n].arms;
    unsigned arms=(a[0]!=0)+(a[1]!=0)+(a[2]!=0)+(a[3]!=0);
    if (arms==2 && (a[0] || a[2]) && (a[1] || a[3])) {
        /* Join the bands at a corner without extending either arm past the
         * outside edge of its partner. Double corners pair corresponding rails. */
        bool right=a[1]!=0,down=a[2]!=0;
        unsigned vx=a[0]?a[0]:a[2],hy=a[1]?a[1]:a[3];
        if (vx==3 && w<(uint32_t)(4*thin)) vx=1;
        if (hy==3 && h<(uint32_t)(4*thin)) hy=1;
        unsigned nx=vx==3?2:1,ny=hy==3?2:1;
        int32_t wx=vx==2?2*thin:thin,wy=hy==2?2*thin:thin;
        for (unsigned ix=0;ix<nx;ix++) for (unsigned iy=0;iy<ny;iy++) {
            if (nx==2 && ny==2 && iy!=(right==down?ix:1-ix)) continue;
            int32_t lx=cx+(nx==2?(ix?thin:-2*thin):-wx/2);
            int32_t ly=cy+(ny==2?(iy?thin:-2*thin):-wy/2);
            if (((int32_t)x>=lx && (int32_t)x<lx+wx &&
                 (down?(int32_t)y>=ly:(int32_t)y<ly+wy)) ||
                ((int32_t)y>=ly && (int32_t)y<ly+wy &&
                 (right?(int32_t)x>=lx:(int32_t)x<lx+wx))) return 255;
        }
        return 0;
    }
    unsigned horizontal=a[1]>a[3]?a[1]:a[3],vertical=a[0]>a[2]?a[0]:a[2];
    int32_t hx=horizontal==3?2*thin:horizontal==2?thin:thin/2;
    int32_t vy=vertical==3?2*thin:vertical==2?thin:thin/2;
    if (a[0] && (int32_t)y<=cy+hx && stroke((int32_t)x,cx,a[0],thin,(int32_t)w)) ink=true;
    if (a[2] && (int32_t)y>=cy-hx && stroke((int32_t)x,cx,a[2],thin,(int32_t)w)) ink=true;
    if (a[3] && (int32_t)x<=cx+vy && stroke((int32_t)y,cy,a[3],thin,(int32_t)h)) ink=true;
    if (a[1] && (int32_t)x>=cx-vy && stroke((int32_t)y,cy,a[1],thin,(int32_t)h)) ink=true;
    unsigned dash=text_boxes[n].dash;
    if (ink && dash) {
        uint32_t position=(a[0]||a[2])?y:x,extent=(a[0]||a[2])?h:w;
        if (position && position+1<extent && (position*(2*dash-1)/extent)%2) ink=false;
    }
    return ink?255:0;
}
