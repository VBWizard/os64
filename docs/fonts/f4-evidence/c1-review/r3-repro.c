/* Follow-up probes use the real toolkit/backend; only the host environment
 * comes from the supplied suite. Output is an observation, not a verdict. */
#define main upstream_test_main
#include F4_C1_HOST_TEST
#undef main

static int r3_ink_left(const canvas_t *c)
{
    int left=SURF_W;
    for (int y=0;y<SURF_H;++y) for (int x=0;x<SURF_W;++x)
        if (c->px[y*SURF_W+x]!=0xff000000 && x<left) left=x;
    return left;
}
static size_t r3_difference(const canvas_t *a,const canvas_t *b)
{
    size_t n=0;
    for (size_t i=0;i<SURF_W*SURF_H;++i) n+=a->px[i]!=b->px[i];
    return n;
}
static os64_font_status_t r3_parent_plan(os64_ui_t *ui,void *user,void **out)
{
    os64_ui_widget_stage_bounds(ui->root,(os64_gui_rect_t){40,20,160,220});
    os64_ui_stack_vertical_staged(ui,ui->root);
    *out=user;
    return OS64_FONT_OK;
}
static void r3_nop(os64_ui_t *ui,void *user,void *plan)
{ (void)ui;(void)user;(void)plan; }
int main(int argc,char **argv)
{
    if (argc!=3) return 2;
    os64_ui_t ui={0};
    ui.theme=(os64_ui_theme_t){.pad=6,.gap=4,.button_h=28,
        .button_fg=0xffffffff,.button_face=0xff000000,.button_border=0xff000000};
    os64_text_context_t *text=os64_ui_font_context(&ui);
    os64_font_set_t *set=outline_set(text,argv[2],"DejaVuSans.ttf",24);
    if (!set) return 2;
    os64_ui_widget_t root,child;
    if (!strcmp(argv[1],"caption")) {
        os64_ui_button(&root,"iiii",NULL,NULL);
        root.bounds=(os64_gui_rect_t){0,0,200,50};
    } else {
        os64_ui_panel(&root);root.bounds=(os64_gui_rect_t){0,0,200,240};
        os64_ui_label(&child,"nested");os64_ui_add_child(&root,&child);
        os64_ui_font_planner(&ui,r3_parent_plan,r3_nop,r3_nop,&root);
    }
    os64_ui_set_root(&ui,&root);
    os64_font_consumer_t c;os64_ui_font_consumer(&ui,&c);
    int status=os64_font_adopt(set,&c,1,NULL);os64_font_set_release(set);
    if (status!=OS64_FONT_OK) return 2;
    if (!strcmp(argv[1],"caption")) {
        root.text="WWWW";
        canvas_t bad,good;canvas_init(&bad,0xff000000);canvas_init(&good,0xff000000);
        os64_draw_ctx_t draw={.surf=bad.s};
        unsigned long before=allocations;
        deny_countdown=0; /* Refuse only the width layout's first allocation. */
        root.cls->paint(&root,&draw,&ui.theme);deny_countdown=-1;
        unsigned long attempts=allocations-before;
        draw.surf=good.s;root.cls->paint(&root,&draw,&ui.theme);
        printf("caption mutation iiii -> WWWW; one allocation denied: attempts=%lu, ink left denied=%d normal=%d, differing pixels=%zu\n",
               attempts,r3_ink_left(&bad),r3_ink_left(&good),r3_difference(&bad,&good));
    } else {
        printf("adoption=%d parent=(%d,%d,%d,%d) child=(%d,%d,%d,%d)\n",status,
               root.bounds.x,root.bounds.y,root.bounds.w,root.bounds.h,
               child.bounds.x,child.bounds.y,child.bounds.w,child.bounds.h);
        printf("expected child x=%d y=%d width=%d from committed parent + padding\n",
               root.bounds.x+ui.theme.pad,root.bounds.y+ui.theme.pad,root.bounds.w-2*ui.theme.pad);
    }
    printf("teardown=%d\n",os64_ui_font_release(&ui));
    return 0;
}
