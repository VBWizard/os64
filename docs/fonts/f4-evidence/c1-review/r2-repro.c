#define main upstream_test_main
#define os64_malloc test_original_malloc
#define os64_free test_original_free
#include F4_C1_HOST_TEST
#undef main
#undef os64_malloc
#undef os64_free

static size_t calls, fail_at, live_blocks;
static bool refuse_all;
void *os64_malloc(size_t n)
{
    ++calls;
    if (refuse_all || calls == fail_at) return NULL;
    void *p = malloc(n ? n : 1);
    if (p) ++live_blocks;
    return p;
}
void os64_free(void *p) { if (p) --live_blocks; free(p); }
static int width(os64_ui_t *ui, const char *s)
{
    int32_t w = -1;
    os64_font_status_t st = os64_ui_text_measure(ui, OS64_FONT_ROLE_UI, s, strlen(s), &w);
    if (st != OS64_FONT_OK) { fprintf(stderr, "unexpected measure status %d\n", st); exit(2); }
    return w;
}
static const char *r2_list_label(size_t index, void *user)
{ (void)index; (void)user; return "WWWW"; }
/* The height this adoption will give the list, PUBLISHED where widget
 * preparation can read it instead of written into live bounds at commit —
 * which is the mechanism R2c asked for. libui applies it at commit. */
static os64_font_status_t resize_plan(os64_ui_t *ui, void *user, void **out)
{
    (void)ui;
    os64_ui_listbox_t *list = user;
    os64_gui_rect_t grown = list->w.bounds;
    grown.h = 58;
    os64_ui_widget_stage_bounds(&list->w, grown);
    *out = user;
    return OS64_FONT_OK;
}
static void resize_commit(os64_ui_t *ui, void *user, void *plan)
{ (void)ui; (void)user; (void)plan; }
static void resize_discard(os64_ui_t *ui, void *user, void *plan)
{ (void)ui; (void)user; (void)plan; }
static void manually_free_list(os64_ui_listbox_t *list)
{
    for (size_t i=0; i<list->row_run_count; ++i) os64_ui_run_release(list->row_runs[i]);
    os64_free(list->row_runs); list->row_runs=NULL; list->row_run_count=0;
}
static os64_ui_theme_t theme(void)
{
    return (os64_ui_theme_t){.button_face=0xff000000, .button_border=0xff000000,
        .button_fg=0xffffffff, .field_bg=0xff000000, .field_fg=0xffffffff,
        .field_border=0xff000000, .font_h=16, .font_w=8, .button_h=28};
}
static int ink_left(const canvas_t *c)
{
    int left=SURF_W;
    for (int y=0;y<SURF_H;++y) for (int x=0;x<SURF_W;++x)
        if (c->px[y*SURF_W+x]!=0xff000000 && x<left) left=x;
    return left;
}
static size_t pixel_difference(const canvas_t *a, const canvas_t *b)
{
    size_t n=0;for (size_t i=0;i<SURF_W*SURF_H;++i) n+=a->px[i]!=b->px[i];return n;
}
int main(int argc, char **argv)
{
    if (argc!=3) return 2;
    const char *mode=argv[1], *dir=argv[2];
    if (!strcmp(mode,"tabs")) {
        size_t wrong=0;
        for (size_t n=1;n<=30;++n) {
            os64_ui_t ui={0};
            os64_text_context_t *ctx=os64_ui_font_context(&ui);
            os64_font_set_t *set=outline_set(ctx,dir,"DejaVuSans.ttf",16);
            if (!set) return 2;
            os64_font_consumer_t c;os64_ui_font_consumer(&ui,&c);
            fail_at=calls+n;
            int status=os64_font_adopt(set,&c,1,NULL);
            fail_at=0;
            if (status==OS64_FONT_OK) {
                int space=width(&ui," "),tab=width(&ui,"\t");
                if (tab!=8*space) {++wrong;printf("deny adoption allocation %zu: status=OK space=%d tab=%d expected=%d\n",n,space,tab,8*space);}
            }
            os64_font_set_release(set);
            if (os64_ui_font_release(&ui)!=OS64_FONT_OK) return 2;
        }
        printf("successful adoptions with wrong tab interval=%zu; live allocator blocks=%zu\n",wrong,live_blocks);
        return 0;
    }
    os64_ui_t ui={0};ui.theme=theme();
    os64_text_context_t *ctx=os64_ui_font_context(&ui);
    os64_font_set_t *set=outline_set(ctx,dir,"DejaVuSans.ttf",!strcmp(mode,"list-resize")?8:24);
    if (!set) return 2;
    if (!strcmp(mode,"busy-state")) {
        os64_ui_font_bind(&ui,set);
        int before=width(&ui,"WWWW");
        int status=os64_ui_font_release(&ui);
        bool same=os64_ui_font_set(&ui)==set;
        printf("release=%d (7=BUSY), active identity preserved=%d, width before=%d after=%d\n",status,same,before,width(&ui,"WWWW"));
        os64_font_set_release(set);os64_ui_font_release(&ui);
    } else if (!strcmp(mode,"button")) {
        os64_ui_widget_t button;os64_ui_button(&button,"WWWW",NULL,NULL);
        button.bounds=(os64_gui_rect_t){0,0,200,50};os64_ui_set_root(&ui,&button);
        os64_font_consumer_t c;os64_ui_font_consumer(&ui,&c);
        int status=os64_font_adopt(set,&c,1,NULL);
        if (status!=OS64_FONT_OK) return 2;
        os64_font_set_release(set);
        canvas_t good,bad;canvas_init(&good,0xff000000);canvas_init(&bad,0xff000000);
        os64_draw_ctx_t draw={.surf=good.s};
        size_t before=calls;button.cls->paint(&button,&draw,&ui.theme);size_t normal_allocs=calls-before;
        draw.surf=bad.s;refuse_all=true;before=calls;
        button.cls->paint(&button,&draw,&ui.theme);size_t denied_allocs=calls-before;refuse_all=false;
        printf("adoption=%d, first actual button paint allocations=%zu, denied paint allocation attempts=%zu\n",status,normal_allocs,denied_allocs);
        printf("ink left normal=%d denied=%d, differing pixels=%zu, retained run=%d\n",ink_left(&good),ink_left(&bad),pixel_difference(&good,&bad),button.run!=NULL);
        printf("teardown=%d\n",os64_ui_font_release(&ui));
    } else {
        os64_ui_listbox_t list;os64_ui_listbox(&list,3,r2_list_label,NULL,NULL);
        list.w.bounds=(os64_gui_rect_t){0,0,200,!strcmp(mode,"list-resize")?22:60};
        os64_ui_set_root(&ui,&list.w);
        if (!strcmp(mode,"list-resize")) os64_ui_font_planner(&ui,resize_plan,resize_commit,resize_discard,&list);
        os64_font_consumer_t c;os64_ui_font_consumer(&ui,&c);
        int status=os64_font_adopt(set,&c,1,NULL);if (status!=OS64_FONT_OK) return 2;
        os64_font_set_release(set);
        printf("adoption=%d, visible rows=%d, retained row runs=%zu\n",status,os64_ui_listbox_rows(&list,&ui.theme),list.row_run_count);
        if (!strcmp(mode,"list-resize")) {
            canvas_t good,bad;canvas_init(&good,0xff000000);canvas_init(&bad,0xff000000);
            os64_draw_ctx_t draw={.surf=bad.s};refuse_all=true;size_t before=calls;
            list.w.cls->paint(&list.w,&draw,&ui.theme);size_t denied_calls=calls-before;refuse_all=false;
            draw.surf=good.s;before=calls;list.w.cls->paint(&list.w,&draw,&ui.theme);
            printf("first paint denied allocation attempts=%zu; normal paint allocations=%zu; differing pixels=%zu\n",denied_calls,calls-before,pixel_difference(&good,&bad));
        }
        int first=os64_ui_font_release(&ui),second=os64_ui_font_release(&ui);
        printf("no caller-owned sets/runs: teardown=%d, retry=%d, list runs still retained=%zu, binding live bytes=%zu\n",first,second,list.row_run_count,os64_ui_font_live_bytes(&ui));
        manually_free_list(&list);
        printf("after probe manually frees list internals, teardown=%d\n",os64_ui_font_release(&ui));
    }
    printf("live allocator blocks=%zu\n",live_blocks);return 0;
}
