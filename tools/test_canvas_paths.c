/* Run C1/C2 ownership tests against the enlarged state before path checks. */
#define main canvas_baseline_main
#include "test_canvas_core.c"
#undef main

static canvas_context_t *drawing(Owner *o)
{
    canvas_options_t opts=options(o);opts.geometry=os64_freetype_geometry_v1();
    canvas_context_t *c;OK(canvas_create(&opts,32,32,&c));return c;
}
static void move(canvas_context_t *c,double x,double y)
{ /* Commands deliberately use the public ABI. */
    os64_geometry_command_t v={.verb=OS64_GEOMETRY_MOVE,.p={{x,y}}};OK(canvas_path_command(c,&v));
}
static void line(canvas_context_t *c,double x,double y)
{ os64_geometry_command_t v={.verb=OS64_GEOMETRY_LINE,.p={{x,y}}};OK(canvas_path_command(c,&v)); }
static void paths(void)
{
    Owner o={0};canvas_context_t *c=drawing(&o);
    move(c,2,2);line(c,12,2);
    os64_geometry_map_t m={1,0,0,1,0,10};OK(canvas_transform(c,&m,true));
    line(c,12,2);line(c,2,2);OK(canvas_fill(c,false));
    pixel(c,5,5,0xff000000);pixel(c,5,15,0);
    OK(canvas_save(c));OK(canvas_begin_path(c));OK(canvas_path_rect(c,0,0,4,4));OK(canvas_restore(c));
    OK(canvas_set_fill(c,0xffff0000));OK(canvas_fill(c,false));pixel(c,1,11,0xffff0000);
    /* Restore did not replace the current path; fill did not transform it twice. */
    OK(canvas_reset(c,32,32));
    move(c,4,16);line(c,20,16);
    m=(os64_geometry_map_t){4,0,0,1,0,0};OK(canvas_transform(c,&m,true));
    os64_geometry_stroke_t pen={4,10,OS64_GEOMETRY_ROUND_CAP,OS64_GEOMETRY_MITER};
    OK(canvas_set_line(c,&pen));OK(canvas_stroke(c));
    pixel(c,0,16,0xff000000);pixel(c,24,16,0xff000000);pixel(c,10,20,0);
    OK(canvas_reset(c,32,32));
    OK(canvas_path_rect(c,2,2,20,20));OK(canvas_path_rect(c,6,6,8,8));
    OK(canvas_fill(c,true));pixel(c,3,3,0xff000000);pixel(c,8,8,0);
    OK(canvas_reset(c,32,32));
    OK(canvas_ellipse(c,16,16,10,6,0,0,6.283185307179586,false));OK(canvas_fill(c,false));
    pixel(c,16,16,0xff000000);pixel(c,16,8,0);pixel(c,4,16,0);
    OK(canvas_begin_path(c));pen.width=4;OK(canvas_set_line(c,&pen));move(c,2,2);OK(canvas_arc_to(c,20,2,20,20,4));
    OK(canvas_set_stroke(c,0xff0000,1));OK(canvas_stroke(c));
    pixel(c,10,2,0xffff0000);
    OK(canvas_reset(c,32,32));
    OK(canvas_ellipse(c,16,16,10,10,0,0,6.283185307179586,true));OK(canvas_fill(c,false));
    pixel(c,16,16,0xff000000);
    OK(canvas_reset(c,32,32));pen=(os64_geometry_stroke_t){40,10,OS64_GEOMETRY_ROUND_CAP,OS64_GEOMETRY_MITER};
    OK(canvas_set_line(c,&pen));OK(canvas_stroke_rect(c,20,16,10,0));pixel(c,5,16,0);
    pen.cap=OS64_GEOMETRY_BUTT;pen.join=OS64_GEOMETRY_ROUND_JOIN;OK(canvas_set_line(c,&pen));
    OK(canvas_stroke_rect(c,20,16,10,0));pixel(c,5,16,0xff000000);
    OK(canvas_reset(c,32,32));move(c,16,16);
    os64_geometry_command_t zero={.verb=OS64_GEOMETRY_CUBIC,.p={{16,16},{16,16},{16,16}}};
    OK(canvas_path_command(c,&zero));pen.cap=OS64_GEOMETRY_ROUND_CAP;OK(canvas_set_line(c,&pen));
    OK(canvas_stroke(c));pixel(c,16,16,0);
    OK(canvas_destroy(c));CHECK(!o.live);
}
static void clips(void)
{
    Owner o={0};canvas_context_t *c=drawing(&o);
    OK(canvas_path_rect(c,2,2,20,20));OK(canvas_clip(c,false));
    OK(canvas_save(c));OK(canvas_begin_path(c));OK(canvas_path_rect(c,8,8,20,20));OK(canvas_clip(c,false));
    OK(canvas_fill_rect(c,0,0,32,32));pixel(c,10,10,0xff000000);pixel(c,4,4,0);pixel(c,24,24,0);
    OK(canvas_restore(c));OK(canvas_set_fill(c,0xffff0000));OK(canvas_fill_rect(c,0,0,32,32));
    pixel(c,4,4,0xffff0000);pixel(c,24,24,0);
    OK(canvas_set_alpha(c,0));OK(canvas_set_composite(c,DRAW2D_DESTINATION_IN));
    OK(canvas_clear_rect(c,3,3,2,2));pixel(c,3,3,0);pixel(c,6,6,0xffff0000);
    OK(canvas_reset(c,32,32));OK(canvas_fill_rect(c,0,0,32,32));pixel(c,24,24,0xff000000);
    OK(canvas_begin_path(c));OK(canvas_clip(c,false));OK(canvas_clear_rect(c,0,0,32,32));pixel(c,24,24,0xff000000);
    OK(canvas_destroy(c));CHECK(!o.live);
}
static void composite_oracles(void)
{
    /* Independent Porter-Duff factors in floating point. The implementation
     * uses 16-bit integer intermediates and rounds at the ARGB8 boundary. */
    for(unsigned op=DRAW2D_SOURCE_OVER;op<=DRAW2D_LIGHTER;++op)for(unsigned cv=0;cv<256;cv+=51) {
        uint32_t pixel_value=0x80402010;
        draw2d_surface_t surface={&pixel_value,1,1,4,4,DRAW2D_ARGB8_PREMULTIPLIED};
        uint8_t coverage=cv,clip=173;
        CHECK(draw2d_paint_mask(&surface,&coverage,&clip,1,(draw2d_solid_t){0x80804020},
            (draw2d_composite_t){op,32768},1,NULL)==DRAW2D_OK);
        double sa=(128.0/255)*(32768.0/65535)*(cv/255.0),da=128.0/255,fs=1,fd=1;
        switch(op) {
        case DRAW2D_SOURCE_OVER:fd=1-sa;break;
        case DRAW2D_CLEAR:fs=0;fd=1-cv/255.0;break;
        case DRAW2D_DESTINATION_OUT:fs=0;fd=1-sa;break;
        case DRAW2D_SOURCE_IN:fs=da;fd=0;break;
        case DRAW2D_SOURCE_OUT:fs=1-da;fd=0;break;
        case DRAW2D_SOURCE_ATOP:fs=da;fd=1-sa;break;
        case DRAW2D_DESTINATION_OVER:fs=1-da;break;
        case DRAW2D_DESTINATION_IN:fs=0;fd=sa;break;
        case DRAW2D_DESTINATION_ATOP:fs=1-da;fd=sa;break;
        case DRAW2D_COPY:fd=0;break;
        case DRAW2D_XOR:fs=1-da;fd=1-sa;break;
        default:break;
        }
        double source[]={sa,sa*128/255,sa*64/255,sa*32/255};
        double dest[]={128.0/255,64.0/255,32.0/255,16.0/255};
        for(unsigned i=0;i<4;++i) {
            double blended=fmin(1,source[i]*fs+dest[i]*fd);
            int expected=(int)floor((blended*173/255+dest[i]*(1-173.0/255))*255+.5);
            int actual=(pixel_value>>(24-8*i))&255;
            CHECK(abs(expected-actual)<=1);
        }
    }
    Owner o={0};canvas_context_t *c=drawing(&o);
    OK(canvas_fill_rect(c,0,0,32,32));OK(canvas_set_composite(c,DRAW2D_DESTINATION_OUT));
    OK(canvas_set_alpha(c,.5));OK(canvas_fill_rect(c,4,4,8,8));
    pixel(c,6,6,0x7f000000);pixel(c,1,1,0xff000000);
    OK(canvas_set_alpha(c,1));OK(canvas_set_composite(c,DRAW2D_COPY));
    OK(canvas_set_fill(c,0xffff0000));OK(canvas_fill_rect(c,4,4,8,8));
    pixel(c,6,6,0xffff0000);pixel(c,1,1,0);
    OK(canvas_destroy(c));CHECK(!o.live);
}
static void refusal(void)
{
    size_t cuts=0;
    for(unsigned mode=0;mode<2;++mode) {
        for(size_t cut=1;cut<=512;++cut) {
            Owner o={0};canvas_context_t *c=drawing(&o);
            move(c,2,16);line(c,28,16);
            double dash[]={2,2};OK(canvas_set_dash(c,dash,2));
            os64_geometry_stroke_t pen={4,10,OS64_GEOMETRY_ROUND_CAP,OS64_GEOMETRY_ROUND_JOIN};
            OK(canvas_set_line(c,&pen));OK(canvas_ack_damage(c));
            o.limit=mode;o.fail_at=o.calls+cut;
            canvas_status_t s=canvas_stroke(c);
            if(s!=CANVAS_OK) {
                CHECK(s==CANVAS_NO_MEMORY||s==CANVAS_LIMIT);
                for(unsigned i=0;i<1024;++i)CHECK(bitmap(c).pixels[i]==0);
                draw2d_damage_t d;OK(canvas_damage(c,&d));CHECK(d.x1==0);
            }
            o.fail_at=0;
            OK(canvas_destroy(c));CHECK(!o.live);
            if(s==CANVAS_OK){cuts=cut-1;break;}
        }
    }
    CHECK(cuts>10);printf("Canvas path allocation cuts: %zu per refusal mode\n",cuts);
}
static void clip_refusal(void)
{
    for(unsigned mode=0;mode<2;++mode)for(size_t cut=1;cut<128;++cut) {
        Owner o={0};canvas_context_t *c=drawing(&o);
        OK(canvas_path_rect(c,0,0,16,16));OK(canvas_clip(c,false));OK(canvas_save(c));
        OK(canvas_begin_path(c));OK(canvas_path_rect(c,8,8,20,20));
        o.limit=mode;o.fail_at=o.calls+cut;
        canvas_status_t s=canvas_clip(c,false);o.fail_at=0;
        CHECK(s==CANVAS_OK||s==CANVAS_NO_MEMORY||s==CANVAS_LIMIT);
        OK(canvas_fill_rect(c,0,0,32,32));
        pixel(c,4,4,s==CANVAS_OK?0:0xff000000);pixel(c,12,12,0xff000000);pixel(c,24,24,0);
        OK(canvas_restore(c));OK(canvas_set_fill(c,0xffff0000));OK(canvas_fill_rect(c,0,0,32,32));
        pixel(c,4,4,0xffff0000);OK(canvas_destroy(c));CHECK(!o.live);
        if(s==CANVAS_OK)break;
        CHECK(cut<127);
    }
    Owner o={0};canvas_options_t opts=options(&o);opts.geometry=os64_freetype_geometry_v1();opts.max_commands=3;
    canvas_context_t *c;OK(canvas_create(&opts,32,32,&c));move(c,4,16);line(c,28,16);
    CHECK(canvas_path_rect(c,0,0,32,32)==CANVAS_LIMIT);
    os64_geometry_stroke_t pen={4,10,OS64_GEOMETRY_BUTT,OS64_GEOMETRY_MITER};OK(canvas_set_line(c,&pen));
    OK(canvas_stroke(c));pixel(c,16,16,0xff000000);pixel(c,16,4,0);OK(canvas_destroy(c));CHECK(!o.live);
}
static void compact_reference(void)
{
    os64_geometry_backend_t full=*os64_freetype_geometry_v1();full.bounds=NULL;
    for(unsigned trial=0;trial<12;++trial) {
        Owner a={0},b={0};canvas_options_t oa=options(&a),ob=options(&b);
        oa.geometry=os64_freetype_geometry_v1();ob.geometry=&full;
        canvas_context_t *c[2];OK(canvas_create(&oa,96,80,&c[0]));OK(canvas_create(&ob,96,80,&c[1]));
        for(unsigned i=0;i<2;++i) {
            OK(canvas_set_fill(c[i],0xff208050));OK(canvas_fill_rect(c[i],0,0,96,80));
            if(trial&1) {
                OK(canvas_path_rect(c[i],20.3,10.7,48.2,50.4));OK(canvas_clip(c[i],false));
            }
            OK(canvas_begin_path(c[i]));
            os64_geometry_map_t m={1.25,.3,-.2,.8,7.3,-3.2};OK(canvas_transform(c[i],&m,true));
            OK(canvas_ellipse(c[i],trial==10?-100:35.25,30.4,11.3,5.7,.4,0,6.283185307179586,false));
            OK(canvas_set_alpha(c[i],.6));OK(canvas_set_fill(c[i],0xc0804020));
            OK(canvas_set_composite(c[i],trial==DRAW2D_CLEAR?DRAW2D_SOURCE_OVER:trial));
            if(trial==DRAW2D_CLEAR)OK(canvas_clear_rect(c[i],20,20,16,12));
            else if(trial%3==0) {
                os64_geometry_stroke_t line={3.3,8,OS64_GEOMETRY_ROUND_CAP,OS64_GEOMETRY_MITER};
                double dash[]={2.3,3.1};OK(canvas_set_line(c[i],&line));OK(canvas_set_dash(c[i],dash,2));
                OK(canvas_stroke(c[i]));
            } else OK(canvas_fill(c[i],trial&1));
        }
        canvas_bitmap_t x=bitmap(c[0]),y=bitmap(c[1]);
        CHECK(x.capacity==y.capacity && !memcmp(x.pixels,y.pixels,x.capacity));
        for(unsigned i=0;i<2;++i){OK(canvas_destroy(c[i]));}
        CHECK(!a.live&&!b.live);
    }
}
static void compact_clip_refusal(void)
{
    for(unsigned mode=0;mode<2;++mode)for(size_t cut=1;cut<128;++cut) {
        Owner o={0};canvas_context_t *c=drawing(&o);
        OK(canvas_fill_rect(c,0,0,32,32));
        OK(canvas_path_rect(c,2.2,3.3,20.5,20.5));OK(canvas_clip(c,false));
        OK(canvas_begin_path(c));OK(canvas_ellipse(c,16,16,3,3,0,0,6.283185307179586,false));
        OK(canvas_set_fill(c,0xffff0000));OK(canvas_ack_damage(c));
        o.limit=mode;o.fail_at=o.calls+cut;
        canvas_status_t status=canvas_fill(c,false);o.fail_at=0;
        CHECK(status==CANVAS_OK || status==CANVAS_NO_MEMORY || status==CANVAS_LIMIT);
        if(status!=CANVAS_OK) {
            canvas_bitmap_t v=bitmap(c);for(size_t i=0;i<32*32;++i)CHECK(v.pixels[i]==0xff000000);
            draw2d_damage_t d;OK(canvas_damage(c,&d));CHECK(!d.x1);
        } else pixel(c,16,16,0xffff0000);
        OK(canvas_destroy(c));CHECK(!o.live);
        if(status==CANVAS_OK)break;
        CHECK(cut<127);
    }
}
static void compact_coverage(void)
{
    Owner o={0};canvas_context_t *c=drawing(&o);
    OK(canvas_reset(c,2048,1024));
    OK(canvas_begin_path(c));
    OK(canvas_ellipse(c,1000.25,500.5,5,5,0,0,2*3.141592653589793,false));
    /* A small shape must fit scratch far below a full bitmap mask. */
    o.budget=o.live+65536;
    OK(canvas_fill(c,false));
    pixel(c,1000,500,0xff000000);pixel(c,1000,510,0);
    o.budget=0;OK(canvas_destroy(c));CHECK(!o.live);
}
static void outside_fills(void)
{
    Owner o={0};canvas_context_t *c=drawing(&o);
    const double centers[][2]={{286000,-148000},{-1e8,16},{16,1e8},{16,-1e8},{1e8,16}};
    for(unsigned i=0;i<5;i++) {
        OK(canvas_begin_path(c));
        OK(canvas_ellipse(c,centers[i][0],centers[i][1],3000,3000,0,0,2*3.141592653589793,false));
        OK(canvas_ack_damage(c));
        size_t calls=o.calls;
        OK(canvas_fill(c,i%2));
        CHECK(o.calls==calls); /* No backend allocation for the discarded fill. */
        draw2d_damage_t damage;OK(canvas_damage(c,&damage));CHECK(!damage.x1);
        pixel(c,16,16,0);
    }
    /* All endpoints outside does not prove that the fill is outside. */
    OK(canvas_begin_path(c));OK(canvas_path_rect(c,-10,-10,52,52));
    OK(canvas_fill(c,false));pixel(c,16,16,0xff000000);
    OK(canvas_reset(c,32,32));OK(canvas_begin_path(c));move(c,-8,4);
    os64_geometry_command_t curve={.verb=OS64_GEOMETRY_CUBIC,.p={{40,4},{40,28},{-8,28}}};
    OK(canvas_path_command(c,&curve));OK(canvas_fill(c,false));pixel(c,8,16,0xff000000);
    /* Copy and clip act on the destination even with an offscreen source. */
    OK(canvas_begin_path(c));OK(canvas_path_rect(c,40,4,8,8));
    OK(canvas_set_composite(c,DRAW2D_COPY));OK(canvas_fill(c,false));pixel(c,8,16,0);
    OK(canvas_reset(c,32,32));OK(canvas_begin_path(c));OK(canvas_path_rect(c,40,4,8,8));
    OK(canvas_clip(c,false));OK(canvas_fill_rect(c,0,0,32,32));pixel(c,8,16,0);
    /* Offscreen geometry can still cast a visible shadow or expand a stroke. */
    OK(canvas_reset(c,32,32));OK(canvas_begin_path(c));OK(canvas_path_rect(c,40,4,8,8));
    OK(canvas_set_shadow_color(c,0x0000ff,1));OK(canvas_set_shadow_value(c,1,-32));
    OK(canvas_fill(c,false));pixel(c,10,6,0xff0000ff);
    OK(canvas_reset(c,32,32));OK(canvas_begin_path(c));move(c,36,4);line(c,36,28);
    os64_geometry_stroke_t pen={16,10,OS64_GEOMETRY_BUTT,OS64_GEOMETRY_MITER};
    OK(canvas_set_line(c,&pen));OK(canvas_stroke(c));pixel(c,30,16,0xff000000);
    /* Device-space points, not the transform subsequently selected for paint. */
    OK(canvas_reset(c,32,32));
    os64_geometry_map_t m={1,0,0,1,1000000,0};OK(canvas_transform(c,&m,true));
    OK(canvas_begin_path(c));OK(canvas_path_rect(c,0,0,8,8));
    m=(os64_geometry_map_t){1,0,0,1,0,0};OK(canvas_transform(c,&m,true));
    OK(canvas_fill(c,false));pixel(c,4,4,0);
    OK(canvas_begin_path(c));OK(canvas_path_rect(c,2,2,8,8));
    m.tx=1000000;OK(canvas_transform(c,&m,true));OK(canvas_fill(c,false));pixel(c,4,4,0xff000000);
    OK(canvas_destroy(c));CHECK(!o.live);
}
#include "test_canvas_images.inc"
#include "test_canvas_paints.inc"
#include "test_canvas_shadows.inc"
#include "test_canvas_retained_path.inc"
int main(void)
{
    canvas_baseline_main();outside_fills();compact_coverage();compact_reference();compact_clip_refusal();paths();clips();composite_oracles();refusal();clip_refusal();images();image_refusals();paints();
    shadows();retained_paths();shadow_refusal();
    printf("Canvas paths: %zu total assertions passed\n",checks);return 0;
}
