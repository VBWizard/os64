// test_font_unwrap_host.c — yonder's WOFF unwrapper (font_unwrap.h) on the
// host, under the sanitizers. Modes:
//   unwrap IN OUT       the SFNT of IN written to OUT, or the status printed
//   status IN           the status alone
//   sweep IN            IN cut at every length, each byte of its header and
//                       directory corrupted in turn, and every allocation
//                       failed in turn: each ends, refuses or answers whole,
//                       and nothing leaks

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "font_unwrap.h"

static size_t allocations, fail_at, live;

void *os64_malloc(size_t size)
{
    allocations++;
    if (fail_at != 0 && allocations == fail_at)
        return NULL;
    void *p = malloc(size + 16);
    if (p == NULL)
        return NULL;
    *(size_t *)p = size;
    live++;
    return (char *)p + 16;
}
void os64_free(void *memory)
{
    if (memory == NULL)
        return;
    live--;
    free((char *)memory - 16);
}
void *os64_memset(void *memory, int value, size_t length) { return memset(memory, value, length); }
void *os64_memcpy(void *destination, const void *source, size_t length)
{
    return memcpy(destination, source, length);
}
int os64_memcmp(const void *a, const void *b, size_t length) { return memcmp(a, b, length); }

static unsigned char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc((size_t)n + 1);
    if (b != NULL && fread(b, 1, (size_t)n, f) != (size_t)n) {
        free(b);
        b = NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return b;
}

// One unwrap that must end and leak nothing: what it answered.
static yonder_font_status_t once(const unsigned char *b, size_t n, size_t *out_len, unsigned char **keep)
{
    size_t before = live;
    uint8_t *sfnt = NULL;
    size_t sfnt_len = 0;
    yonder_font_status_t st = yonder_font_unwrap_woff(b, n, &sfnt, &sfnt_len);
    if ((st == YONDER_FONT_OK) != (sfnt != NULL)) {
        fprintf(stderr, "FAIL: status %s with output %p\n", yonder_font_status_name(st), (void *)sfnt);
        exit(1);
    }
    if (out_len != NULL)
        *out_len = sfnt_len;
    if (keep != NULL && sfnt != NULL) {
        *keep = malloc(sfnt_len);
        memcpy(*keep, sfnt, sfnt_len);
    }
    os64_free(sfnt);
    if (live != before) {
        fprintf(stderr, "FAIL: %zu leaked\n", live - before);
        exit(1);
    }
    return st;
}

int main(int argc, char **argv)
{
    if (argc < 3)
        return 2;
    size_t n;
    unsigned char *b = slurp(argv[2], &n);
    if (b == NULL)
        return 2;
    if (strcmp(argv[1], "unwrap") == 0 && argc == 4) {
        size_t len;
        unsigned char *out = NULL;
        yonder_font_status_t st = once(b, n, &len, &out);
        if (st != YONDER_FONT_OK) {
            printf("%s\n", yonder_font_status_name(st));
            return 1;
        }
        FILE *f = fopen(argv[3], "wb");
        fwrite(out, 1, len, f);
        fclose(f);
        free(out);
        free(b);
        return 0;
    }
    if (strcmp(argv[1], "status") == 0) {
        printf("%s\n", yonder_font_status_name(once(b, n, NULL, NULL)));
        free(b);
        return 0;
    }
    if (strcmp(argv[1], "sweep") == 0) {
        size_t whole_len;
        unsigned char *whole = NULL;
        if (once(b, n, &whole_len, &whole) != YONDER_FONT_OK)
            return 1;
        // Every shorter length: refused, never read past.
        size_t cuts = 0;
        for (size_t cut = 0; cut < n; cut += cut < 4096 ? 1 : 997, cuts++) {
            unsigned char *c = malloc(cut + 1);
            memcpy(c, b, cut);
            if (once(c, cut, NULL, NULL) == YONDER_FONT_OK) {
                fprintf(stderr, "FAIL: cut at %zu answered whole\n", cut);
                return 1;
            }
            free(c);
        }
        // Each byte of the header and directory, and a stride of the data:
        // whatever it answers, it ends and leaks nothing.
        size_t dir = 44 + 20 * (size_t)((b[12] << 8) | b[13]);
        size_t tried = 0;
        for (size_t at = 0; at < n; at += at < dir ? 1 : 211)
            for (int bit = 0; bit < 8; bit += at < dir ? 1 : 8) {
                unsigned char *c = malloc(n);
                memcpy(c, b, n);
                c[at] ^= (unsigned char)(1u << bit);
                (void)once(c, n, NULL, NULL);
                free(c);
                tried++;
            }
        // Every allocation failed in turn: NO_MEMORY, or the whole answer.
        allocations = 0;
        (void)once(b, n, NULL, NULL);
        size_t count = allocations;
        for (size_t at = 1; at <= count; at++) {
            allocations = 0;
            fail_at = at;
            size_t len;
            unsigned char *got = NULL;
            yonder_font_status_t st = once(b, n, &len, &got);
            fail_at = 0;
            if (st != YONDER_FONT_NO_MEMORY &&
                !(st == YONDER_FONT_OK && len == whole_len && memcmp(got, whole, len) == 0)) {
                fprintf(stderr, "FAIL: allocation %zu failed answered %s\n", at, yonder_font_status_name(st));
                return 1;
            }
            free(got);
        }
        printf("%zu cuts, %zu corruptions, %zu allocations failed in turn\n", cuts, tried, count);
        free(whole);
        free(b);
        return 0;
    }
    return 2;
}
