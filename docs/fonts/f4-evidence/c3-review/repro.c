/* Review: the real Scribe on the host harness's canvas and in-memory files. */
#include F4_C3_SCRIBE_TEST

static size_t review_caret_pixels(void)
{
    os64_gui_rect_t r=g.view.w.bounds;
    size_t n=0;
    g.view.w.cls->paint(&g.view.w,&g.ctx,&g.ui.theme);
    for(int y=r.y;y<r.y+r.h;y++)for(int x=r.x;x<r.x+r.w;x++)
        n+=sh_px[y*SH_W+x]==g.ui.theme.text_caret;
    return n;
}

static size_t review_staged_caret,review_staged_row;
static os64_font_status_t review_barrier(void *user,void *plan)
{
    review_staged_caret=ui_test_run_bytes(g.view.w.run_staged);
    review_staged_row=g.view.row_runs_staged_count?ui_test_run_bytes(g.view.row_runs_staged[0]):0;
    sh_barrier(user,plan);
    return OS64_FONT_OK;
}

static os64_font_status_t review_deny_commit(void *user,void *plan)
{
    review_barrier(user,plan);
    ui_test_deny_all=true;
    return OS64_FONT_OK;
}

int main(int argc,char **argv)
{
    sh_dir=argc>1?argv[1]:"userland/libfreetype/fixtures";
    size_t len=20000;char *doc=malloc(len+1);
    memset(doc,'W',len);doc[len]='\n';
    sh_scribe(16,doc,len+1);
    os64_ui_textview_goto(&g.ui,&g.view,0,12000,false);
    for(int i=0;i<100;i++)SH_RIGHT(0);
    size_t from=g.view.win_from,caret=g.view.cur_col;
    int64_t left=g.view.left_px;
    size_t before=review_caret_pixels();
    help_toggle();SH_DOWN();help_toggle();
    printf("help round trip: caret %zu -> %zu, kept origin %zu -> %zu, left %lld -> %lld, caret pixels %zu -> %zu\n",
           caret,g.view.cur_col,from,g.view.win_from,(long long)left,(long long)g.view.left_px,
           before,review_caret_pixels());
    sh_close();free(doc);
    for(int denied=0;denied<2;denied++) {
        len=2u*1024u*1024u;doc=malloc(len+1);
        memset(doc,'W',len);doc[len]='\n';
        sh_scribe(16,doc,len+1);
        g.view.window_bytes=1536u*1024u;
        os64_ui_textview_goto(&g.ui,&g.view,0,len/2,false);
        sh_barrier_allocs=0;
        os64_font_status_t adopted=sh_adopt(24,denied?review_deny_commit:review_barrier);
        unsigned long attempted=sh_barrier_allocs?allocations-sh_barrier_allocs:0;
        ui_test_deny_all=false;
        printf("prepared caret run bytes=%zu, prepared row run bytes=%zu\n",review_staged_caret,review_staged_row);
        printf("LIMIT-window font adoption (deny commit=%d): status=%d, post-barrier allocations=%lu, extent=%lld, total=%lld, caret run bytes=%zu, row run bytes=%zu\n",
               denied,adopted,attempted,(long long)g.max_width,(long long)g.hscroll.total,
               ui_test_run_bytes(g.view.w.run),g.view.row_run_count?ui_test_run_bytes(g.view.row_runs[0]):0);
        printf("caret pixels after adoption: %zu\n",review_caret_pixels());
        sh_close();free(doc);
    }
    for(size_t i=0;i<8;i++)free(sh_fs[i].bytes);
    printf("Harness checks: %lu; failures: %d\n",checks,failures);
    return failures?1:0;
}
