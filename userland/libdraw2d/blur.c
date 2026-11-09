#include <draw2d/blur.h>

static void pass(const draw2d_mask16_t *m, uint16_t *tmp, uint32_t radius,
                 bool rows, uint16_t edge)
{
    size_t stride=m->stride/sizeof(uint16_t);
    uint32_t lines=rows?m->height:m->width, len=rows?m->width:m->height;
    size_t step=rows?1:stride;
    uint64_t span=2*(uint64_t)radius+1;
    for (uint32_t line=0;line<lines;line++) {
        uint16_t *v=m->pixels+(rows?(size_t)line*stride:line);
        uint64_t count=(uint64_t)radius+1;
        if(count>len)count=len;
        // Count the constant exterior analytically, so a huge radius does
        // not spend time walking pixels that are outside this row/column.
        uint64_t sum=(span-count)*edge;
        for(uint64_t i=0;i<count;i++)sum+=v[(size_t)i*step];
        for(uint32_t i=0;i<len;i++) {
            tmp[i]=(uint16_t)((sum+span/2)/span);
            uint64_t in=(uint64_t)i+radius+1;
            sum+=in>=len?edge:v[(size_t)in*step];
            sum-=i<radius?edge:v[(size_t)(i-radius)*step];
        }
        for(uint32_t i=0;i<len;i++)v[(size_t)i*step]=tmp[i];
    }
}

draw2d_status_t draw2d_blur_mask16(const draw2d_mask16_t *m, uint32_t radius,
                                 uint16_t edge, uint16_t *scratch,
                                 size_t scratch_capacity, size_t max_pixels)
{
    if(!m)return DRAW2D_BAD_ARGUMENT;
    if(!m->width||!m->height)return DRAW2D_OK;
    if((uint64_t)m->width*m->height>max_pixels)return DRAW2D_LIMIT;
    uint64_t row=(uint64_t)m->width*sizeof(uint16_t);
    if(!m->pixels || (uintptr_t)m->pixels%_Alignof(uint16_t) ||
       m->stride%sizeof(uint16_t) || row>m->stride || row>m->capacity ||
       m->height-1>(m->capacity-(size_t)row)/m->stride)return DRAW2D_BAD_ARGUMENT;
    size_t used=(size_t)(m->height-1)*m->stride+(size_t)row;
    if(used>UINTPTR_MAX-(uintptr_t)m->pixels)return DRAW2D_BAD_ARGUMENT;
    if(!radius)return DRAW2D_OK;
    uint64_t need=(uint64_t)(m->width>m->height?m->width:m->height)*sizeof(uint16_t);
    if(need>scratch_capacity)return DRAW2D_LIMIT;
    if(!scratch || (uintptr_t)scratch%_Alignof(uint16_t) ||
       need>UINTPTR_MAX-(uintptr_t)scratch)return DRAW2D_BAD_ARGUMENT;
    uintptr_t begin=(uintptr_t)m->pixels, temp=(uintptr_t)scratch;
    if(temp<begin+used && begin<temp+(size_t)need)return DRAW2D_BAD_ARGUMENT;
    for(int k=0;k<3;k++) {
        pass(m,scratch,radius,true,edge);
        pass(m,scratch,radius,false,edge);
    }
    return DRAW2D_OK;
}
