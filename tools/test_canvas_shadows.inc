static void shadow_style(canvas_context_t *c,double x,double y,double blur)
{
    OK(canvas_set_shadow_color(c,0x0000ff,1));
    OK(canvas_set_shadow_value(c,0,blur));OK(canvas_set_shadow_value(c,1,x));OK(canvas_set_shadow_value(c,2,y));
}
/* Floating-point Porter-Duff reference, independent of draw2d's integer
 * compositor. Round after each of the two bitmap operations. */
static uint32_t shadow_reference(uint32_t destination,const double source[4],unsigned op,double clip)
{
    double da=(destination>>24)/255.0,sa=source[0],fs=1,fd=1;
    switch(op) {
    case DRAW2D_SOURCE_OVER:fd=1-sa;break;
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
    uint32_t result=0;
    for(unsigned i=0;i<4;i++) {
        double dest=((destination>>(24-i*8))&255)/255.0;
        unsigned value=(unsigned)floor((fmin(1,source[i]*fs+dest*fd)*clip+dest*(1-clip))*255+.5);
        result|=value<<(24-i*8);
    }
    return result;
}
static void shadow_operators(void)
{
    Owner o={0};canvas_context_t *c=drawing(&o);
    for(unsigned clipped=0;clipped<2;clipped++)for(unsigned op=DRAW2D_SOURCE_OVER;op<=DRAW2D_LIGHTER;op++) {
        if(op==DRAW2D_CLEAR)continue;
        OK(canvas_reset(c,32,32));OK(canvas_set_fill(c,0x80804020));OK(canvas_fill_rect(c,0,0,32,32));
        uint32_t destination=bitmap(c).pixels[0];
        if(clipped){OK(canvas_path_rect(c,3.5,0,10,32));OK(canvas_clip(c,false));}
        shadow_style(c,4,0,0);OK(canvas_set_shadow_color(c,0x0000ff,.7));
        OK(canvas_set_fill(c,0x80ff0000));OK(canvas_set_alpha(c,.6));OK(canvas_set_composite(c,op));
        OK(canvas_fill_rect(c,2,2,8,8));
        for(unsigned y=0;y<32;y++)for(unsigned x=0;x<32;x++) {
            double sa=x>=6&&x<14&&y>=2&&y<10?(128.0/255)*.6*.7:0;
            double shadow[]={sa,0,0,sa};
            double a=x>=2&&x<10&&y>=2&&y<10?(128.0/255)*.6:0;
            double source[]={a,a,0,0};
            double clip=!clipped?1:x==3||x==13?128.0/255:x>3&&x<13?1:0;
            uint32_t expected=shadow_reference(shadow_reference(destination,shadow,op,clip),source,op,clip);
            uint32_t actual=bitmap(c).pixels[y*32+x];
            for(unsigned channel=0;channel<4;channel++)
                CHECK(abs((int)((expected>>(channel*8))&255)-(int)((actual>>(channel*8))&255))<=2);
        }
    }
    OK(canvas_destroy(c));CHECK(!o.live);
}
static void shadows(void)
{
    shadow_operators();
    Owner o={0};canvas_context_t *c=drawing(&o);
    CHECK(state(c).shadow_alpha==0&&state(c).shadow_blur==0);
    shadow_style(c,8,2,0);OK(canvas_set_fill(c,0xffff0000));OK(canvas_fill_rect(c,2,2,4,4));
    pixel(c,3,3,0xffff0000);pixel(c,11,5,0xff0000ff);pixel(c,9,5,0);
    OK(canvas_save(c));shadow_style(c,-2,-3,6.5);OK(canvas_restore(c));
    CHECK(state(c).shadow_x==8&&state(c).shadow_y==2&&state(c).shadow_blur==0);
    OK(canvas_set_shadow_value(c,0,-1));OK(canvas_set_shadow_value(c,1,NAN));OK(canvas_set_shadow_value(c,2,INFINITY));
    CHECK(state(c).shadow_x==8&&state(c).shadow_y==2&&state(c).shadow_blur==0);
    OK(canvas_reset(c,32,32));shadow_style(c,12,0,0);
    OK(canvas_fill_rect(c,-8,2,4,4));pixel(c,5,3,0xff0000ff);pixel(c,1,3,0);
    OK(canvas_reset(c,32,32));shadow_style(c,8,0,0);
    OK(canvas_path_rect(c,8,0,8,16));OK(canvas_clip(c,false));
    OK(canvas_fill_rect(c,2,2,4,4));pixel(c,3,3,0);pixel(c,11,3,0xff0000ff);
    OK(canvas_reset(c,32,32));shadow_style(c,8,0,0);
    os64_geometry_map_t m={2,0,0,2,0,0};OK(canvas_transform(c,&m,true));
    OK(canvas_fill_rect(c,2,2,2,2));pixel(c,5,5,0xff000000);pixel(c,13,5,0xff0000ff);pixel(c,21,5,0);
    OK(canvas_reset(c,32,32));shadow_style(c,4.5,0,0);
    OK(canvas_fill_rect(c,2,2,1,1));pixel(c,6,2,0x80000080);pixel(c,7,2,0x80000080);
    OK(canvas_reset(c,32,32));shadow_style(c,8,0,0);OK(canvas_set_shadow_color(c,0x0000ff,.5));
    OK(canvas_set_alpha(c,.5));OK(canvas_set_fill(c,0x80ff0000));OK(canvas_fill_rect(c,2,2,2,2));
    pixel(c,2,2,0x40400000);pixel(c,10,2,0x20000020);
    OK(canvas_clear_rect(c,2,2,2,2));pixel(c,2,2,0);pixel(c,10,2,0x20000020);
    OK(canvas_reset(c,32,32));shadow_style(c,4,0,0);OK(canvas_set_composite(c,DRAW2D_COPY));
    OK(canvas_fill_rect(c,2,2,2,2));pixel(c,2,2,0xff000000);pixel(c,6,2,0);
    OK(canvas_reset(c,32,32));shadow_style(c,0,0,6);
    OK(canvas_fill_rect(c,-3,10,2,8));CHECK((bitmap(c).pixels[12*32]>>24)>0);
    OK(canvas_reset(c,32,32));shadow_style(c,0,0,.75);
    OK(canvas_fill_rect(c,10,10,1,1));CHECK((bitmap(c).pixels[10*32+9]>>24)>0);
    OK(canvas_set_shadow_value(c,0,1e300));CHECK(state(c).shadow_blur==1e300);
    OK(canvas_fill_rect(c,10,10,1,1));
    OK(canvas_reset(c,32,32));shadow_style(c,8,0,0);OK(canvas_set_smoothing(c,false,0));
    uint32_t source[]={0xffff0000,0,0xffff0000,0};
    draw2d_surface_t image={source,2,2,8,sizeof(source),DRAW2D_ARGB8_PREMULTIPLIED};
    OK(canvas_draw_image(c,&image,true,0,0,2,2,2,2,2,2));
    pixel(c,2,2,0xffff0000);pixel(c,10,2,0xff0000ff);pixel(c,11,2,0);
    OK(canvas_reset(c,32,32));shadow_style(c,0,8,0);
    double args[]={0,0,8,0};canvas_gradient_t *g=NULL;
    OK(canvas_gradient_create(options(&o).memory,0,args,&g));
    OK(canvas_gradient_add_stop(g,0,0));OK(canvas_gradient_add_stop(g,1,0xffff0000));OK(canvas_set_gradient(c,g,false));
    OK(canvas_fill_rect(c,0,0,8,4));
    for(unsigned x=0;x<8;x++)CHECK((bitmap(c).pixels[x]>>24)==(bitmap(c).pixels[8*32+x]>>24));
    canvas_gradient_release(g);
    OK(canvas_reset(c,32,32));shadow_style(c,0,8,0);move(c,2,2);line(c,10,2);OK(canvas_stroke(c));
    CHECK((bitmap(c).pixels[10*32+5]>>24)>0);
    OK(canvas_destroy(c));CHECK(!o.live);
}
static void shadow_refusal(void)
{
    for(unsigned mode=0;mode<2;mode++)for(size_t cut=1;cut<128;cut++) {
        Owner o={0};canvas_context_t *c=drawing(&o);OK(canvas_set_fill(c,0xff00ff00));OK(canvas_fill_rect(c,0,0,32,32));
        shadow_style(c,3,2,7);OK(canvas_set_fill(c,0xffff0000));OK(canvas_ack_damage(c));
        o.limit=mode;o.fail_at=o.calls+cut;
        canvas_status_t s=canvas_fill_rect(c,3,3,8,8);o.fail_at=0;
        if(s!=CANVAS_OK) {
            CHECK(s==CANVAS_LIMIT||s==CANVAS_NO_MEMORY);
            for(size_t i=0;i<1024;i++)CHECK(bitmap(c).pixels[i]==0xff00ff00);
            draw2d_damage_t damage;OK(canvas_damage(c,&damage));CHECK(!damage.x1);
        }
        OK(canvas_destroy(c));CHECK(!o.live);
        if(s==CANVAS_OK){CHECK(cut>10);printf("Canvas shadow allocation cuts: %zu mode %u\n",cut-1,mode);break;}
        CHECK(cut<127);
    }
}
