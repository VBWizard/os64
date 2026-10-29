static void images(void)
{
    Owner o={0};canvas_context_t *c=drawing(&o);
    uint32_t src[]={0xffff0000,0xff00ff00,0xff0000ff,0x00000000};
    draw2d_surface_t image={src,2,2,8,sizeof(src),DRAW2D_ARGB8_STRAIGHT};
    CHECK(canvas_origin_clean(c));OK(canvas_set_smoothing(c,false,0));
    OK(canvas_draw_image(c,&image,true,0,0,2,2,0,0,8,8));
    pixel(c,0,0,src[0]);pixel(c,7,0,src[1]);pixel(c,0,7,src[2]);pixel(c,7,7,0);
    /* Overlapping self-copy must read the pre-draw image for the whole operation. */
    canvas_bitmap_t b=bitmap(c);
    draw2d_surface_t self={(void *)b.pixels,b.width,b.height,b.stride,b.capacity,DRAW2D_ARGB8_PREMULTIPLIED};
    OK(canvas_draw_image(c,&self,true,0,0,8,8,2,0,8,8));
    pixel(c,2,0,src[0]);pixel(c,5,0,src[0]);pixel(c,6,0,src[1]);pixel(c,9,0,src[1]);
    OK(canvas_reset(c,32,32));OK(canvas_set_smoothing(c,false,0));
    OK(canvas_draw_image(c,&image,true,-1,0,2,1,0,0,8,4));
    pixel(c,3,0,0);pixel(c,4,0,src[0]);pixel(c,7,3,src[0]);pixel(c,8,0,0);
    OK(canvas_draw_image(c,&image,true,2,1,-2,-1,16,8,-8,-4));
    pixel(c,8,4,src[0]);pixel(c,15,4,src[1]);
    OK(canvas_reset(c,32,32));OK(canvas_set_smoothing(c,false,0));
    os64_geometry_map_t rotation={0,1,-1,0,16,4};OK(canvas_transform(c,&rotation,true));
    OK(canvas_draw_image(c,&image,true,0,0,2,2,0,0,8,8));
    pixel(c,15,4,src[0]);pixel(c,15,11,src[1]);pixel(c,8,4,src[2]);
    OK(canvas_reset(c,32,32));OK(canvas_set_smoothing(c,true,2));
    OK(canvas_draw_image(c,&image,true,0,0,2,2,0,0,4,4));
    uint32_t mixed=bitmap(c).pixels[1+32];
    CHECK((mixed>>24)>200&&(mixed>>24)<255);CHECK(((mixed>>16)&255)>100&&((mixed>>16)&255)<170);
    /* Clip and COPY: uncovered pixels inside the clip become transparent. */
    OK(canvas_reset(c,32,32));OK(canvas_set_fill(c,0xffffffff));OK(canvas_fill_rect(c,0,0,32,32));
    OK(canvas_path_rect(c,2,2,10,10));OK(canvas_clip(c,false));OK(canvas_set_composite(c,DRAW2D_COPY));
    OK(canvas_draw_image(c,&image,false,0,0,2,2,4,4,2,2));
    pixel(c,0,0,0xffffffff);pixel(c,2,2,0);pixel(c,4,4,src[0]);CHECK(!canvas_origin_clean(c));
    OK(canvas_save(c));OK(canvas_restore(c));CHECK(!canvas_origin_clean(c));
    OK(canvas_reset(c,32,32));CHECK(canvas_origin_clean(c));
    os64_geometry_map_t singular={0};OK(canvas_transform(c,&singular,true));
    OK(canvas_draw_image(c,&image,false,0,0,2,2,0,0,8,8));CHECK(!canvas_origin_clean(c));
    pixel(c,0,0,0);OK(canvas_reset(c,32,32));
    OK(canvas_save(c));OK(canvas_set_smoothing(c,false,2));OK(canvas_restore(c));
    canvas_state_t state;OK(canvas_state(c,&state));CHECK(state.image_smoothing&&state.image_quality==0);
    OK(canvas_destroy(c));CHECK(!o.live);
}
static void image_refusals(void)
{
    for(unsigned mode=0;mode<2;++mode)for(size_t cut=1;cut<128;++cut) {
        Owner o={0};canvas_context_t *c=drawing(&o);
        OK(canvas_set_fill(c,0xff73512f));OK(canvas_fill_rect(c,0,0,32,32));
        canvas_bitmap_t b=bitmap(c);
        draw2d_surface_t self={(void *)b.pixels,b.width,b.height,b.stride,b.capacity,DRAW2D_ARGB8_PREMULTIPLIED};
        OK(canvas_ack_damage(c));o.limit=mode;o.fail_at=o.calls+cut;
        canvas_status_t status=canvas_draw_image(c,&self,false,0,0,16,16,8,8,16,16);
        if(status!=CANVAS_OK) {
            CHECK(status==CANVAS_LIMIT||status==CANVAS_NO_MEMORY);CHECK(canvas_origin_clean(c));
            for(unsigned i=0;i<1024;++i)CHECK(bitmap(c).pixels[i]==0xff73512f);
            draw2d_damage_t d;OK(canvas_damage(c,&d));CHECK(d.x0==d.x1);
        }
        o.fail_at=0;OK(canvas_destroy(c));CHECK(!o.live);
        if(status==CANVAS_OK)break;
        CHECK(cut<127);
    }
}
