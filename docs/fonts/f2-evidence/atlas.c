#include <stdio.h>
#include "text_internal.h"
int main(void) {
    const unsigned w=16*28,h=10*36;
    printf("P6\n%u %u\n255\n",w,h);
    for(unsigned y=0;y<h;y++)for(unsigned x=0;x<w;x++) {
        unsigned cx=x%28,cy=y%36,cp=0x2500+(y/36)*16+x/28;
        unsigned char p[3]={240,238,229};
        if(cx<24 && cy<32) {
            unsigned char ink=text_box_pixel(cp,cx,cy,24,32);
            p[0]=p[1]=p[2]=255-ink;
        }
        fwrite(p,1,3,stdout);
    }
    return 0;
}
