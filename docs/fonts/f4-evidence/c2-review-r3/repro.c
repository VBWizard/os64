/* Recovery inside one paint: use the real widget, run engine and canvas. */
#define main upstream_test_main
#include F4_C2_HOST_TEST
#undef main

int main(void)
{
    os64_ui_t ui={0};os64_ui_textview_t tv;canvas_t c;
    ui_test_view_theme(&ui.theme);
    os64_ui_textview(&tv,&kUiTestBuf,NULL,NULL,NULL);
    tv.w.bounds=(os64_gui_rect_t){0,0,300,20};
    os64_ui_set_root(&ui,&tv.w);
    tv.top=3;tv.cur_line=3;tv.cur_col=5;tv.w.focused=true;
    tv.sel=true;tv.sel_line=3;tv.sel_col=3;
    /* Reserve a row slot, leaving the binding allocation as the next one. */
    tv.row_runs=calloc(1,sizeof(*tv.row_runs));tv.row_run_count=1;
    canvas_init(&c,0xff000000);os64_draw_ctx_t ctx={0};ctx.surf=c.s;
    deny_countdown=0;
    tv.w.cls->paint(&tv.w,&ctx,&ui.theme);
    deny_countdown=-1;
    int32_t run_x=-1;
    os64_ui_run_caret(tv.row_runs[0],5,false,&run_x);
    int caret_x=-1,last_selected=-1;
    for(int y=2;y<18;y++)for(int x=2;x<298;x++) {
        if(c.px[y*SURF_W+x]==ui.theme.text_caret && (caret_x<0||x<caret_x))caret_x=x;
        if(c.px[y*SURF_W+x]==ui.theme.text_sel_bg && x>last_selected)last_selected=x;
    }
    printf("Transient binding refusal: retained run=%d run caret x=%d painted caret x=%d selected rightmost x=%d (origin=2)\n",
           tv.row_runs[0]!=NULL,run_x,caret_x,last_selected);
    canvas_init(&c,0xff000000);
    tv.w.cls->paint(&tv.w,&ctx,&ui.theme);
    caret_x=-1;last_selected=-1;
    for(int y=2;y<18;y++)for(int x=2;x<298;x++) {
        if(c.px[y*SURF_W+x]==ui.theme.text_caret && (caret_x<0||x<caret_x))caret_x=x;
        if(c.px[y*SURF_W+x]==ui.theme.text_sel_bg && x>last_selected)last_selected=x;
    }
    printf("Next paint, unchanged bytes: caret x=%d selected rightmost x=%d\n",caret_x,last_selected);
    printf("teardown=%d\n",os64_ui_font_release(&ui));
    {
        os64_ui_t field_ui={0};os64_ui_textfield_t tf;char buf[32];
        ui_test_view_theme(&field_ui.theme);
        field_ui.theme.field_fg=0xffffffff;field_ui.theme.field_bg=0xff000000;
        os64_ui_textfield(&tf,buf,sizeof(buf),NULL,NULL,NULL);
        tf.w.bounds=(os64_gui_rect_t){0,0,150,32};
        os64_ui_set_root(&field_ui,&tf.w);tf.w.focused=true;
        ui_test_deny_all=true;
        os64_ui_textfield_set(&field_ui,&tf,"caf\xc3\xa9");
        ui_test_deny_all=false;
        canvas_init(&c,0xff000000);
        deny_countdown=0;
        tf.w.cls->paint(&tf.w,&ctx,&field_ui.theme);
        deny_countdown=-1;
        caret_x=-1;
        for(int y=8;y<24;y++)for(int x=4;x<146;x++)
            if(c.px[y*SURF_W+x]==field_ui.theme.text_caret && (caret_x<0||x<caret_x))caret_x=x;
        printf("Field transient binding refusal: bitmap text ends at x=44, painted caret x=%d retained run=%d\n",caret_x,tf.w.run!=NULL);
        printf("field teardown=%d\n",os64_ui_font_release(&field_ui));
    }
    return 0;
}
