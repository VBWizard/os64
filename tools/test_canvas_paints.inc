static void paints(void)
{
    Owner o={0};canvas_context_t *c=drawing(&o),*other=drawing(&o);
    canvas_gradient_t *g=NULL;
    os64_geometry_memory_t memory=options(&o).memory;
    double line_coords[]={0,0,32,0};
    OK(canvas_gradient_create(memory,0,line_coords,&g));OK(canvas_set_gradient(c,g,false));
    OK(canvas_fill_rect(c,0,0,32,32));pixel(c,2,2,0);
    OK(canvas_gradient_add_stop(g,0,0xffff0000));OK(canvas_gradient_add_stop(g,1,0xff0000ff));
    OK(canvas_fill_rect(c,0,0,32,32));pixel(c,0,0,0xfffb0004);pixel(c,31,0,0xff0400fb);
    OK(canvas_save(c));OK(canvas_set_fill(c,0xff000000));OK(canvas_set_gradient(other,g,false));
    OK(canvas_gradient_add_stop(g,.5,0xff00ff00));OK(canvas_gradient_add_stop(g,.5,0xffffff00));
    OK(canvas_restore(c));OK(canvas_fill_rect(c,0,0,32,32));OK(canvas_fill_rect(other,0,0,32,32));
    canvas_bitmap_t a,b;OK(canvas_bitmap(c,&a));OK(canvas_bitmap(other,&b));CHECK(!memcmp(a.pixels,b.pixels,a.capacity));
    CHECK((a.pixels[16]&0xff00)>0xe000);CHECK(canvas_gradient_add_stop(g,NAN,0)==CANVAS_BAD_ARGUMENT);
    OK(canvas_save(c));OK(canvas_set_gradient(c,g,true));canvas_gradient_release(g);
    OK(canvas_reset(c,32,32));OK(canvas_destroy(other));
    double coincident[]={1,1,1,1};OK(canvas_gradient_create(memory,0,coincident,&g));
    OK(canvas_gradient_add_stop(g,0,0xffff0000));OK(canvas_set_gradient(c,g,false));canvas_gradient_release(g);
    OK(canvas_fill_rect(c,0,0,32,32));pixel(c,16,16,0);
    double cone[]={16,16,0,24,16,8};OK(canvas_gradient_create(memory,1,cone,&g));
    OK(canvas_gradient_add_stop(g,0,0xffff0000));OK(canvas_set_gradient(c,g,false));canvas_gradient_release(g);
    OK(canvas_fill_rect(c,0,0,32,32));pixel(c,2,16,0);pixel(c,24,16,0xffff0000);
    OK(canvas_reset(c,32,32));double conic[]={0,16.5,16.5};OK(canvas_gradient_create(memory,2,conic,&g));
    OK(canvas_gradient_add_stop(g,0,0xffff0000));OK(canvas_gradient_add_stop(g,.25,0xff00ff00));
    OK(canvas_gradient_add_stop(g,.5,0xff0000ff));OK(canvas_gradient_add_stop(g,1,0xffff0000));
    OK(canvas_set_gradient(c,g,false));OK(canvas_fill_rect(c,0,0,32,32));
    pixel(c,24,16,0xffff0000);pixel(c,16,24,0xff00ff00);pixel(c,8,16,0xff0000ff);
    canvas_gradient_release(g);
    /* External contours share paint/clip/CTM but cannot replace the path. */
    OK(canvas_reset(c,32,32));OK(canvas_path_rect(c,0,0,4,4));
    os64_geometry_command_t shape[]={
        {.verb=OS64_GEOMETRY_MOVE,.p={{0,0}}},{.verb=OS64_GEOMETRY_LINE,.p={{8,0}}},
        {.verb=OS64_GEOMETRY_LINE,.p={{8,8}}},{.verb=OS64_GEOMETRY_LINE,.p={{0,8}}},
        {.verb=OS64_GEOMETRY_CLOSE}};
    os64_geometry_map_t place={1,0,0,1,16,16};
    OK(canvas_draw_contours(c,shape,5,&place,false));pixel(c,20,20,0xff000000);pixel(c,2,2,0);
    OK(canvas_set_fill(c,0xffff0000));OK(canvas_fill(c,false));pixel(c,2,2,0xffff0000);
    canvas_state_t state;OK(canvas_state(c,&state));CHECK(!strcmp(state.text.font,"10px sans-serif"));
    state.text.size=32;strcpy(state.text.font,"32px serif");OK(canvas_set_text_style(c,&state.text));OK(canvas_save(c));
    state.text.size=15;strcpy(state.text.font,"15px monospace");OK(canvas_set_text_style(c,&state.text));OK(canvas_restore(c));
    OK(canvas_state(c,&state));CHECK(state.text.size==32&&!strcmp(state.text.font,"32px serif"));
    OK(canvas_destroy(c));CHECK(!o.live);
    for(unsigned mode=0;mode<2;++mode)for(size_t cut=1;cut<100;++cut) {
        c=drawing(&o);OK(canvas_gradient_create(memory,0,line_coords,&g));
        OK(canvas_gradient_add_stop(g,0,0xff00ff00));OK(canvas_set_gradient(c,g,false));canvas_gradient_release(g);
        o.limit=mode;o.fail_at=o.calls+cut;
        canvas_status_t s=canvas_draw_contours(c,shape,5,&place,false);o.fail_at=0;
        CHECK(s==CANVAS_OK||s==CANVAS_NO_MEMORY||s==CANVAS_LIMIT);
        pixel(c,20,20,s==CANVAS_OK?0xff00ff00:0);
        OK(canvas_destroy(c));CHECK(!o.live);if(s==CANVAS_OK)break;CHECK(cut<99);
    }
}
