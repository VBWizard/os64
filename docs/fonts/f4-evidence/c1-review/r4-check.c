/* Bounded checks of the two repaired paths and their immediate siblings. */
#define main upstream_test_main
#include F4_C1_HOST_TEST
#undef main

static os64_ui_widget_t r4_root,r4_panel,r4_label;
static os64_font_status_t r4_nested_plan(os64_ui_t *ui,void *user,void **out)
{
    os64_ui_widget_stage_bounds(&r4_root,(os64_gui_rect_t){40,20,160,220});
    os64_ui_stack_vertical_staged(ui,&r4_root);
    os64_ui_stack_vertical_staged(ui,&r4_panel);
    *out=user;
    return OS64_FONT_OK;
}
static void r4_nop(os64_ui_t *ui,void *user,void *plan)
{ (void)ui;(void)user;(void)plan; }
static os64_font_status_t r4_refuse_barrier(void *user,void *plan)
{ (void)user;(void)plan;return OS64_FONT_LIMIT; }
static void r4_nested(const char *dir)
{
    current="nested staged stack and late abort";
    os64_ui_t ui={0};ui.theme.pad=6;ui.theme.gap=4;
    os64_ui_panel(&r4_root);os64_ui_panel(&r4_panel);os64_ui_label(&r4_label,"inside");
    r4_root.bounds=(os64_gui_rect_t){0,0,200,240};
    r4_panel.bounds=(os64_gui_rect_t){6,6,188,90};
    r4_label.bounds=(os64_gui_rect_t){12,12,176,16};
    os64_ui_add_child(&r4_root,&r4_panel);os64_ui_add_child(&r4_panel,&r4_label);
    os64_ui_set_root(&ui,&r4_root);
    os64_gui_rect_t before[]={r4_root.bounds,r4_panel.bounds,r4_label.bounds};
    os64_text_context_t *text=os64_ui_font_context(&ui);
    os64_font_set_t *set=outline_set(text,dir,"DejaVuSans.ttf",24);
    CHECK(set!=NULL);if (!set) return;
    CHECK(os64_ui_font_planner(&ui,r4_nested_plan,r4_nop,r4_nop,&ui)==OS64_FONT_OK);
    os64_font_consumer_t c;os64_ui_font_consumer(&ui,&c);
    c.barrier=r4_refuse_barrier;
    CHECK(os64_font_adopt(set,&c,1,NULL)==OS64_FONT_LIMIT);
    CHECK(memcmp(&r4_root.bounds,&before[0],sizeof(before[0]))==0);
    CHECK(memcmp(&r4_panel.bounds,&before[1],sizeof(before[1]))==0);
    CHECK(memcmp(&r4_label.bounds,&before[2],sizeof(before[2]))==0);
    CHECK(!r4_root.bounds_staged_valid && !r4_panel.bounds_staged_valid && !r4_label.bounds_staged_valid);
    CHECK(r4_label.run_staged==NULL);
    c.barrier=NULL;
    CHECK(os64_font_adopt(set,&c,1,NULL)==OS64_FONT_OK);
    CHECK(r4_panel.bounds.x==46 && r4_panel.bounds.y==26 && r4_panel.bounds.w==148);
    CHECK(r4_label.bounds.x==52 && r4_label.bounds.y==32 && r4_label.bounds.w==136);
    CHECK(r4_panel.bounds.h==90);
    os64_gui_rect_t committed=r4_label.bounds;
    os64_ui_stack_vertical(&ui,&r4_panel);
    CHECK(memcmp(&r4_label.bounds,&committed,sizeof(committed))==0);
    os64_font_set_release(set);
    CHECK(os64_ui_font_release(&ui)==OS64_FONT_OK);
}
static void r4_caption(const char *dir)
{
    current="changed caption allocation sweep";
    for (long denial=0;denial<24;++denial) {
        os64_ui_t ui={0};ui.theme=(os64_ui_theme_t){.pad=6,.button_h=28,
            .button_face=0xff000000,.button_border=0xff000000,.button_fg=0xffffffff};
        os64_ui_widget_t button;os64_ui_button(&button,"iiii",NULL,NULL);
        button.bounds=(os64_gui_rect_t){0,0,200,50};os64_ui_set_root(&ui,&button);
        os64_text_context_t *text=os64_ui_font_context(&ui);
        os64_font_set_t *set=outline_set(text,dir,"DejaVuSans.ttf",24);
        CHECK(set!=NULL);if (!set) return;
        os64_font_consumer_t c;os64_ui_font_consumer(&ui,&c);
        CHECK(os64_font_adopt(set,&c,1,NULL)==OS64_FONT_OK);os64_font_set_release(set);
        canvas_t denied,good,blank;canvas_init(&denied,0xff000000);
        canvas_init(&good,0xff000000);canvas_init(&blank,0xff000000);
        os64_draw_ctx_t ctx={.surf=denied.s};button.text="WWWW";
        deny_countdown=denial;button.cls->paint(&button,&ctx,&ui.theme);deny_countdown=-1;
        ctx.surf=good.s;button.cls->paint(&button,&ctx,&ui.theme);
        CHECK(memcmp(denied.px,good.px,sizeof(good.px))==0 ||
              memcmp(denied.px,blank.px,sizeof(blank.px))==0);
        CHECK(os64_ui_run_matches(button.run,"WWWW",4));
        CHECK(os64_ui_font_release(&ui)==OS64_FONT_OK);
    }
}
int main(int argc,char **argv)
{
    if (argc!=2) return 2;
    r4_nested(argv[1]);r4_caption(argv[1]);
    printf("r4 supplemental: %lu checks, %d failures; 24 caption denial positions, nested commit and barrier abort\n",checks,failures);
    return failures?1:0;
}
