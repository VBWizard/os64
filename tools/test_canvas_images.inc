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

/* The reference backend omits bounds, so it rasterizes complete masks.
 * Compare sampled pixels across transforms, nested clips and compositing. */
static void compact_images(void)
{
    os64_geometry_backend_t full=*os64_freetype_geometry_v1();full.bounds=NULL;
    uint32_t pixels[64];
    for(unsigned i=0;i<64;i++)pixels[i]=(i%3?0xff000000:0x80000000)|((i*37193)&0xffffff);
    draw2d_surface_t image={pixels,8,8,32,sizeof(pixels),DRAW2D_ARGB8_STRAIGHT};
    for(unsigned trial=0;trial<72;trial++) {
        Owner owner[2]={{0},{0}};canvas_context_t *c[2];
        for(unsigned i=0;i<2;i++) {
            canvas_options_t opts=options(&owner[i]);opts.geometry=i?&full:os64_freetype_geometry_v1();
            OK(canvas_create(&opts,96,80,&c[i]));
            OK(canvas_set_fill(c[i],0xff208050));OK(canvas_fill_rect(c[i],0,0,96,80));
            move(c[i],15.25,8.75);line(c[i],75.5,25.2);line(c[i],30.3,68.7);
            OK(canvas_path_command(c[i],&(os64_geometry_command_t){.verb=OS64_GEOMETRY_CLOSE}));OK(canvas_clip(c[i],false));OK(canvas_save(c[i]));
            OK(canvas_begin_path(c[i]));
            OK(canvas_path_rect(c[i],trial%3==0?120:23.4,18.2,31.3,30.7));OK(canvas_clip(c[i],false));
            os64_geometry_map_t m={1.2,.3,-.25,.9,25.7,12.2};OK(canvas_transform(c[i],&m,true));
            OK(canvas_set_smoothing(c[i],trial&1,0));OK(canvas_set_alpha(c[i],.7));
            unsigned op=trial%12;OK(canvas_set_composite(c[i],op==DRAW2D_CLEAR?DRAW2D_SOURCE_OVER:op));
            OK(canvas_draw_image(c[i],&image,true,.5,.25,7.25,7.5,-8,-2,48,48));
            OK(canvas_restore(c[i]));
            OK(canvas_set_alpha(c[i],.4));
            OK(canvas_draw_image(c[i],&image,true,0,0,8,8,31.4,28.7,12.8,13.9));
        }
        canvas_bitmap_t a=bitmap(c[0]),b=bitmap(c[1]);
        CHECK(a.capacity==b.capacity&&!memcmp(a.pixels,b.pixels,a.capacity));
        for(unsigned i=0;i<2;i++){OK(canvas_destroy(c[i]));CHECK(!owner[i].live);}
    }
    Owner o={0};canvas_context_t *c=drawing(&o);OK(canvas_reset(c,2048,1024));
    o.budget=o.live+65536;
    move(c,1000.25,500.25);line(c,1015.5,500.25);line(c,1000.25,515.5);
    OK(canvas_path_command(c,&(os64_geometry_command_t){.verb=OS64_GEOMETRY_CLOSE}));OK(canvas_clip(c,false));
    OK(canvas_draw_image(c,&image,true,0,0,8,8,998,498,24,24));
    CHECK(bitmap(c).pixels[502*2048+1002]!=0);pixel(c,1018,518,0);
    o.budget=0;OK(canvas_destroy(c));CHECK(!o.live);
}

static void image_region_contract(void)
{
    uint32_t source[]={0xffff0000,0xff00ff00,0xff0000ff,0xffffffff},destination[4]={0};
    uint8_t coverage[]={255,255,255,255};
    draw2d_image_t image={{source,2,2,8,sizeof(source),DRAW2D_ARGB8_STRAIGHT},
        {1,0,0,1,-4,-5},false};
    draw2d_surface_t surface={destination,2,2,8,sizeof(destination),DRAW2D_ARGB8_PREMULTIPLIED};
    draw2d_damage_t damage;
    draw2d_composite_t op={DRAW2D_SOURCE_OVER,65535};
    CHECK(draw2d_paint_image_at(&surface,coverage,NULL,4,&image,4,5,op,4,&damage)==DRAW2D_OK);
    CHECK(!memcmp(source,destination,sizeof(source)));
    CHECK(damage.x0==0&&damage.y0==0&&damage.x1==2&&damage.y1==2);
    CHECK(draw2d_paint_image_at(&surface,coverage,NULL,4,&image,4,5,op,3,&damage)==DRAW2D_LIMIT);
    CHECK(!damage.x1&&!memcmp(source,destination,sizeof(source)));
    image.device_to_source.a=DBL_MAX;
    CHECK(draw2d_paint_image_at(&surface,coverage,NULL,4,&image,UINT32_MAX,5,op,4,&damage)==DRAW2D_LIMIT);
    CHECK(!damage.x1&&!memcmp(source,destination,sizeof(source)));
}
static void clipped_image_refusals(void)
{
    uint32_t source=0xff3050a0;
    draw2d_surface_t image={&source,1,1,4,4,DRAW2D_ARGB8_STRAIGHT};
    for(unsigned mode=0;mode<2;mode++)for(size_t cut=1;cut<128;cut++) {
        Owner o={0};canvas_context_t *c=drawing(&o);
        OK(canvas_set_fill(c,0xff73512f));OK(canvas_fill_rect(c,0,0,32,32));
        OK(canvas_path_rect(c,3.3,4.7,12.2,13.5));OK(canvas_clip(c,false));
        OK(canvas_ack_damage(c));o.limit=mode;o.fail_at=o.calls+cut;
        canvas_status_t status=canvas_draw_image(c,&image,false,0,0,1,1,4,5,8,8);
        o.fail_at=0;
        if(status!=CANVAS_OK) {
            CHECK(status==CANVAS_LIMIT||status==CANVAS_NO_MEMORY);CHECK(canvas_origin_clean(c));
            canvas_bitmap_t b=bitmap(c);for(unsigned i=0;i<1024;i++)CHECK(b.pixels[i]==0xff73512f);
            draw2d_damage_t d;OK(canvas_damage(c,&d));CHECK(!d.x1);
        } else {CHECK(!canvas_origin_clean(c));pixel(c,6,7,source);pixel(c,2,2,0xff73512f);}
        OK(canvas_destroy(c));CHECK(!o.live);
        if(status==CANVAS_OK)break;
        CHECK(cut<127);
    }
}
