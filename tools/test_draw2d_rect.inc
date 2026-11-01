/* Compare rectangle output with per-pixel area coverage, including untouched
 * row padding. The oracle has no interior-span specialization. */
static void rectangle_spans(void)
{
    static const double rects[][4] = {
        {0,0,11,9}, {-4,-3,20,18}, {.25,.5,9.5,7.25},
        {10.75,8.75,-10.5,-8.5}, {2,3,1,1}, {2.5,3.5,.25,.25},
        {2.25,0,.5,9}, {0,3.25,11,.5}, {11,0,4,9},
        {-1,-1,DBL_MAX,DBL_MAX}, {DBL_MAX,0,DBL_MAX,1},
        {DBL_MAX,DBL_MAX,-DBL_MAX,-DBL_MAX}, {0,0,0,9}
    };
    static const uint32_t colors[] = {0xff000000,0xffffffff,0xff456789,0x80456789,0x01456789,0};
    static const uint16_t opacities[] = {0,1,32768,65534,65535};
    enum { W=11,H=9,STRIDE=14 };
    uint32_t narrow[STRIDE*H], expected_narrow[STRIDE*H];
    draw2d_pixel_t wide[STRIDE*H], expected_wide[STRIDE*H];
    for (unsigned format=0;format<2;format++)
        for (unsigned mode=0;mode<2;mode++)
            for (size_t color=0;color<sizeof(colors)/sizeof(*colors);color++)
                for (size_t opacity=0;opacity<sizeof(opacities)/sizeof(*opacities);opacity++)
                    for (size_t r=0;r<sizeof(rects)/sizeof(*rects)+128;r++) {
                        double x,y,w,h;
                        if (r<sizeof(rects)/sizeof(*rects)) {
                            x=rects[r][0];y=rects[r][1];w=rects[r][2];h=rects[r][3];
                        } else {
                            x=((int)(r*17%67)-20)/4.;y=((int)(r*11%61)-20)/4.;
                            w=((int)(r*7%97)-48)/4.;h=((int)(r*19%83)-41)/4.;
                        }
                        for (unsigned i=0;i<STRIDE*H;i++) {
                            narrow[i]=expected_narrow[i]=0x80402010u+(i%16);
                            wide[i]=expected_wide[i]=(draw2d_pixel_t){40001,23003,11007,(uint16_t)(9001+i)};
                        }
                        double left=fmin(x,x+w),right=fmax(x,x+w);
                        double top=fmin(y,y+h),bottom=fmax(y,y+h);
                        for (unsigned py=0;py<H;py++) for (unsigned px=0;px<W;px++) {
                            double dx=fmax(0,fmin(right,px+1.)-fmax(left,px));
                            double dy=fmax(0,fmin(bottom,py+1.)-fmax(top,py));
                            uint16_t coverage=(uint16_t)(dx*dy*65535+.5);
                            unsigned at=py*STRIDE+px;
                            draw2d_pixel_t old=format?wide[at]:draw2d_unpack(narrow[at]);
                            draw2d_pixel_t result=mode?draw2d_scale(old,65535-coverage):
                                draw2d_over(draw2d_color(colors[color],opacities[opacity],coverage),old);
                            if (format) expected_wide[at]=result;
                            else expected_narrow[at]=draw2d_pack(result);
                        }
                        size_t unit=format?sizeof(*wide):sizeof(*narrow);
                        draw2d_surface_t surface={format?(void *)wide:(void *)narrow,W,H,STRIDE*unit,
                            ((H-1)*STRIDE+W)*unit,format?DRAW2D_ARGB16_PREMULTIPLIED:DRAW2D_ARGB8_PREMULTIPLIED};
                        draw2d_composite_t op={mode?DRAW2D_CLEAR:DRAW2D_SOURCE_OVER,opacities[opacity]};
                        draw2d_damage_t damage;
                        CHECK(draw2d_paint_rect(&surface,x,y,w,h,(draw2d_solid_t){colors[color]},op,W*H,&damage)==DRAW2D_OK);
                        CHECK(!memcmp(surface.pixels,format?(void *)expected_wide:(void *)expected_narrow,STRIDE*H*unit));
                        uint64_t area=(uint64_t)(damage.x1-damage.x0)*(damage.y1-damage.y0);
                        if (area) {
                            memcpy(format?(void *)expected_wide:(void *)expected_narrow,surface.pixels,STRIDE*H*unit);
                            CHECK(draw2d_paint_rect(&surface,x,y,w,h,(draw2d_solid_t){colors[color]},op,area-1,&damage)==DRAW2D_LIMIT);
                            CHECK(!damage.x0&&!damage.y0&&!damage.x1&&!damage.y1);
                            CHECK(!memcmp(surface.pixels,format?(void *)expected_wide:(void *)expected_narrow,STRIDE*H*unit));
                        }
                    }
}

/* Large interiors exercise the byte lookup with every source alpha and
 * destination byte, including clipping and fractional edge coverage. */
static void rectangle_blend_table(void)
{
    enum { W=65,H=33,STRIDE=68 };
    uint32_t pixels[STRIDE*H], expected[STRIDE*H];
    const uint16_t opacity[]={1,32768,65534,65535};
    const double rectangles[][4]={{0,0,32,32},{0,0,31,33},{0,0,W,H},
                                  {.25,.5,64.5,32.25},{-1,-1,67,35}};
    draw2d_surface_t surface={pixels,W,H,STRIDE*4,((H-1)*STRIDE+W)*4,DRAW2D_ARGB8_PREMULTIPLIED};
    for (unsigned a=0;a<256;a++) for (unsigned o=0;o<4;o++) for (unsigned r=0;r<5;r++) {
        uint32_t color=(a<<24)|0x17b3ef;
        for(unsigned i=0;i<STRIDE*H;i++) {
            unsigned d=i%256;
            pixels[i]=expected[i]=(d<<24)|((d/2)<<16)|((d/3)<<8)|d;
        }
        double x=rectangles[r][0],y=rectangles[r][1],w=rectangles[r][2],h=rectangles[r][3];
        for(unsigned py=0;py<H;py++) for(unsigned px=0;px<W;px++) {
            double cx=fmax(0,fmin(x+w,px+1.)-fmax(x,px));
            double cy=fmax(0,fmin(y+h,py+1.)-fmax(y,py));
            uint16_t coverage=(uint16_t)(cx*cy*65535+.5);
            unsigned at=py*STRIDE+px;
            expected[at]=draw2d_pack(draw2d_over(draw2d_color(color,opacity[o],coverage),
                                               draw2d_unpack(expected[at])));
        }
        CHECK(draw2d_paint_rect(&surface,x,y,w,h,(draw2d_solid_t){color},
              (draw2d_composite_t){DRAW2D_SOURCE_OVER,opacity[o]},W*H,NULL)==DRAW2D_OK);
        CHECK(!memcmp(pixels,expected,sizeof(pixels)));
    }
}
