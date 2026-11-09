/* Where Scribe's open of a 100,000-line log spends its time, on the host:
 * the Scribe harness's real Scribe and stubbed disk, with a clock around
 * reading the file alone, Scribe's own Open, and a font change with the log
 * open. run.sh builds it against a copy of tools/test_scribe_host.c whose
 * main is renamed, so this main is the program's. The same log shape as
 * scribefonttest --log-demo. */
#define _GNU_SOURCE
#include "scribe_harness_copy.c"
#include <time.h>

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static char *make_log(size_t *out_len)
{
    size_t cap = 16u << 20, n = 0;
    char *b = malloc(cap);
    for (unsigned i = 0; i < 100000u; ++i) {
        n += (size_t)snprintf(b + n, cap - n, "[%8u.%03u] core %u: scheduler: task %u ran %u ticks ",
                              i / 7, (i * 37) % 1000, i % 8, (i * 13) % 4096, i % 97);
        unsigned pad = (i * 29) % 64;
        for (unsigned p = 0; p < pad; ++p)
            b[n++] = (char)('a' + (i + p) % 26);
        b[n++] = '\n';
    }
    *out_len = n;
    return b;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    sh_dir = argc > 1 ? argv[1] : "userland/libfreetype/fixtures";
    size_t len;
    char *doc = make_log(&len);

    memset(&g, 0, sizeof(g));
    g.ctx.surf.pixels = sh_px;
    g.ctx.surf.width = SH_W;
    g.ctx.surf.height = SH_H;
    g.ctx.surf.pitch_px = SH_W;
    scribe_build();
    sh_put("/doc", doc, len);

    /* The bytes alone, into a buffer of their own. */
    char err[96];
    sbuf_t alone;
    sbuf_init(&alone);
    double t0 = now_ms();
    sbuf_load(&alone, "/doc", err, sizeof(err));
    double t1 = now_ms();
    size_t lines = alone.count;
    sbuf_free(&alone);

    /* Scribe's Open, under the builtin face; a change to DejaVu Sans with
     * the log open; and Scribe's Open again under that face. */
    double t2 = now_ms();
    do_load("/doc");
    double t3 = now_ms();
    os64_font_set_t *set = outline_set(os64_ui_font_context(&g.ui), sh_dir,
                                       "DejaVuSans.ttf", 16);
    os64_font_consumer_t c;
    os64_ui_font_consumer(&g.ui, &c);
    os64_font_status_t status = os64_font_adopt(set, &c, 1, NULL);
    os64_font_set_release(set);
    double t4 = now_ms();
    do_load("/doc");
    double t5 = now_ms();
    printf("%zu bytes, %zu lines: reading them %.1f ms\n", len, lines, t1 - t0);
    printf("Open, builtin face: %.1f ms\n", t3 - t2);
    printf("change to DejaVu Sans 16 with it open: %.1f ms (%s)\n", t4 - t3,
           status == OS64_FONT_OK ? "adopted" : "refused");
    printf("Open under DejaVu Sans 16: %.1f ms\n", t5 - t4);
    free(doc);
    return 0;
}
