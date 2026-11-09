#include "internal.h"
#include "page/image_source.h"
#include <float.h>

static bool space(char c)
{ return c==' ' || c=='\t' || c=='\r' || c=='\n' || c=='\f'; }

static void consider(os64_page_source_t *best, const os64_page_source_t *one, double ratio)
{
    if (!best->url || (best->density < ratio && one->density > best->density) ||
        (one->density >= ratio && one->density < best->density)) *best = *one;
}

os64_page_source_status_t os64_page_source_select(const char *srcset, const char *src,
    double source_size, double ratio, os64_page_source_t *out)
{
    *out = (os64_page_source_t){0};
    if (!srcset) srcset = "";
    size_t bytes = 0;
    while (srcset[bytes] && bytes <= OS64_PAGE_SRCSET_BYTES_MAX) bytes++;
    if (bytes > OS64_PAGE_SRCSET_BYTES_MAX) return OS64_PAGE_SOURCE_LIMIT;
    const char *p = srcset;
    bool has_width = false, has_one = false;
    unsigned candidates = 0;
    if (!(ratio > 0)) ratio = 1;
    while (*p) {
        while (space(*p) || *p == ',') p++;
        if (!*p) break;
        if (++candidates > OS64_PAGE_SRCSET_CANDIDATES_MAX) {
            *out = (os64_page_source_t){0}; return OS64_PAGE_SOURCE_LIMIT;
        }
        const char *url = p;
        while (*p && !space(*p)) p++;
        size_t length = (size_t)(p-url);
        bool bad = false, width = false, density = false, height = false;
        double w = 0, x = 1;
        if (length && url[length-1] == ',') {
            while (length && url[length-1] == ',') length--;
        } else {
            while (*p && *p != ',') {
                while (space(*p)) p++;
                if (!*p || *p == ',') break;
                const char *start = p;
                bool parens = false;
                while (*p && (parens || (!space(*p) && *p != ','))) {
                    if (*p == '(') parens = true;
                    else if (*p == ')') parens = false;
                    p++;
                }
                size_t n = (size_t)(p-start);
                char number[128]; double v = 0;
                if (n > sizeof(number)) {
                    *out=(os64_page_source_t){0};return OS64_PAGE_SOURCE_LIMIT;
                }
                if (n < 2) { bad = true; continue; }
                os64_memcpy(number, start, n-1); number[n-1] = 0;
                char kind = start[n-1];
                bool integer = true;
                for (size_t i=0;i<n-1;i++) if (number[i]<'0'||number[i]>'9') integer=false;
                if (!p_number_parse_clamped(number, &v) || v < 0) bad = true;
                else if (kind == 'w' && integer && v > 0 && !width && !density) { width=true; w=v; }
                else if (kind == 'x' && !width && !density && !height) { density=true; x=v; }
                else if (kind == 'h' && integer && v > 0 && !height && !density) height=true;
                else bad=true;
            }
            if (*p == ',') p++;
        }
        if (bad || !length || (height && !width)) continue;
        has_width |= width;
        has_one |= !width && x == 1;
        double d = width ? (source_size > 0 ? w/source_size : DBL_MAX) : x;
        if (d > DBL_MAX) d = DBL_MAX;
        os64_page_source_t one = {url,length,d};
        consider(out,&one,ratio);
    }
    if (src && !has_width && !has_one) {
        while (space(*src)) src++;
        size_t n=os64_strlen(src); while(n && space(src[n-1])) n--;
        if(n) { os64_page_source_t one={src,n,1}; consider(out,&one,ratio); }
    }
    return out->url ? OS64_PAGE_SOURCE_OK : OS64_PAGE_SOURCE_EMPTY;
}
