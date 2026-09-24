/* Supplement the unchanged first-round reproductions with combined paths. */
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



int main(int argc, char **argv)
{
    sh_dir = argc > 1 ? argv[1] : "userland/libfreetype/fixtures";
    for (int face = 0; face < 2; ++face) {
        size_t len = 2u * 1024u * 1024u;
        char *doc = malloc(len + 1);
        memset(doc, 'W', len); doc[len] = '\n';
        sh_scribe(16, doc, len + 1);
        g.view.window_bytes = 1536u * 1024u;
        os64_ui_textview_goto(&g.ui, &g.view, 0, len / 2, false);
        CHECK(sh_adopt(24, sh_barrier) == OS64_FONT_OK);
        unsigned long paint_start = allocations;
        size_t before = review_caret_pixels();
        printf("first view paint: allocation attempts=%lu, caret pixels=%zu\n",
               allocations - paint_start, before);
        CHECK(before > 0);
        size_t budget = g.view.win_budget, origin = g.view.win_from;
        int64_t left = g.view.left_px;
        void *caret_run = g.view.w.run;
        void *row_run = g.view.row_runs[0];
        CHECK(sh_adopt(20, sh_refuse) == OS64_FONT_LIMIT);
        CHECK(g.view.win_budget == budget && g.view.win_from == origin);
        CHECK(g.view.left_px == left && g.view.w.run == caret_run &&
              g.view.row_runs[0] == row_run);
        CHECK(review_caret_pixels() == before);
        printf("refused adoption: budget, origin, scroll, run identities and caret preserved\n");
        help_toggle(); SH_DOWN();
        if (face) CHECK(sh_adopt(20, NULL) == OS64_FONT_OK);
        help_toggle();
        size_t after = review_caret_pixels();
        printf("LIMIT + Help (font change=%d): budget %zu -> %zu, origin %zu -> %zu, left %lld -> %lld, caret pixels %zu -> %zu\n",
               face, budget, g.view.win_budget, origin, g.view.win_from,
               (long long)left, (long long)g.view.left_px, before, after);
        CHECK(after > 0 && g.view.cur_col == len / 2);
        CHECK(g.buf.lines[0].len == len &&
              memcmp(g.buf.lines[0].bytes, doc, len) == 0 && !g.buf.dirty);
        sh_close(); free(doc);
    }
    for (size_t i = 0; i < 8; ++i) free(sh_fs[i].bytes);
    printf("Harness checks: %lu; failures: %d\n", checks, failures);
    return failures ? 1 : 0;
}
