static void retained_paths(void)
{
    Owner o={0};canvas_context_t *c=drawing(&o);
    const os64_geometry_command_t open[]={
        {.verb=OS64_GEOMETRY_MOVE,.p={{2,2}}},
        {.verb=OS64_GEOMETRY_LINE,.p={{10,2}}},
        {.verb=OS64_GEOMETRY_LINE,.p={{10,10}}},
        {.verb=OS64_GEOMETRY_LINE,.p={{2,10}}}};
    OK(canvas_path_rect(c,22,22,4,4));
    os64_geometry_map_t m={1,0,0,1,12,0};OK(canvas_transform(c,&m,true));
    OK(canvas_draw_path(c,open,4,CANVAS_PATH_FILL,false));pixel(c,16,4,0xff000000);pixel(c,4,4,0);
    OK(canvas_transform(c,&(os64_geometry_map_t){1,0,0,1,0,0},true));
    OK(canvas_fill(c,false));pixel(c,23,23,0xff000000);
    OK(canvas_reset(c,32,32));
    OK(canvas_draw_path(c,open,4,CANVAS_PATH_STROKE,false));pixel(c,2,6,0);CHECK(bitmap(c).pixels[2*32+6]!=0);
    OK(canvas_reset(c,32,32));OK(canvas_save(c));
    OK(canvas_draw_path(c,open,4,CANVAS_PATH_CLIP,false));OK(canvas_fill_rect(c,0,0,32,32));
    pixel(c,4,4,0xff000000);pixel(c,20,20,0);OK(canvas_restore(c));OK(canvas_fill_rect(c,20,20,2,2));pixel(c,20,20,0xff000000);
    OK(canvas_destroy(c));CHECK(!o.live);
    for(unsigned operation=CANVAS_PATH_FILL;operation<=CANVAS_PATH_CLIP;operation++)
    for(unsigned mode=0;mode<2;mode++)for(size_t cut=1;cut<128;cut++) {
        o=(Owner){0};c=drawing(&o);OK(canvas_set_fill(c,0xff00ff00));OK(canvas_fill_rect(c,0,0,32,32));
        OK(canvas_set_fill(c,0xffff0000));OK(canvas_ack_damage(c));o.limit=mode;o.fail_at=o.calls+cut;
        canvas_status_t status=canvas_draw_path(c,open,4,operation,false);o.fail_at=0;
        if(status!=CANVAS_OK) {
            CHECK(status==CANVAS_NO_MEMORY||status==CANVAS_LIMIT);
            for(unsigned i=0;i<1024;i++)CHECK(bitmap(c).pixels[i]==0xff00ff00);
            draw2d_damage_t damage;OK(canvas_damage(c,&damage));CHECK(!damage.x1);
            OK(canvas_fill_rect(c,20,20,2,2));pixel(c,20,20,0xffff0000);
        }
        OK(canvas_destroy(c));CHECK(!o.live);
        if(status==CANVAS_OK)break;
        CHECK(cut<127);
    }
}
