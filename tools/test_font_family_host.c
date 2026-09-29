#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "os64/font_provider.h"
#include "text_internal.h"
#include "fake_backend.h"

#define CHECK(x) do { ++checks; if (!(x)) {fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); exit(1);} } while (0)
static size_t checks, live, peak, calls, deny;
static void *allocate(void *u, size_t n)
{
    (void)u;
    if (++calls == deny) return NULL;
    void *p = malloc(n);
    if (p) {live += n; if (live > peak) peak = live;}
    return p;
}
static void release(void *u, void *p, size_t n)
{ (void)u; CHECK(live >= n); live -= n; free(p); }
static os64_text_context_t *context(size_t cap)
{
    os64_text_context_t *text = NULL;
    os64_text_options_t options = {.memory = {NULL,allocate,release},
        .backend = os64_fake_font_backend(), .memory_cap = cap};
#ifdef FAMILY_REAL
    options.backend = os64_freetype_backend_v1();
#endif
    CHECK(!os64_font_context_create(&options,&text));
    return text;
}
#ifndef FAMILY_REAL
const os64_font_backend_t *os64_freetype_backend_v1(void) {return os64_fake_font_backend();}
#endif
static uint8_t fake[12] = {'P','M','P','L','P','M','P','L','P','M','P','L'};
static os64_font_family_spec_t specs[3];
static void sources(const char *dir)
{
    (void)dir;
    for (size_t f = 0; f < 3; ++f) for (size_t s = 0; s < 4; ++s) {
        specs[f].styles[s] = (os64_font_source_t){OS64_FONT_SOURCE_OUTLINE,&fake[f*4+s],1};
#ifdef FAMILY_REAL
        const char *family[] = {"Serif","Sans","SansMono"};
        const char *suffix[] = {"","-Bold",f ? "-Oblique" : "-Italic",f ? "-BoldOblique" : "-BoldItalic"};
        char path[1024]; snprintf(path,sizeof(path),"%s/DejaVu%s%s.ttf",dir,family[f],suffix[s]);
        FILE *file = fopen(path,"rb"); CHECK(file);
        CHECK(!fseek(file,0,SEEK_END)); long n = ftell(file); CHECK(n>0); rewind(file);
        uint8_t *bytes = malloc((size_t)n); CHECK(bytes);
        CHECK(fread(bytes,1,(size_t)n,file)==(size_t)n); fclose(file);
        specs[f].styles[s] = (os64_font_source_t){OS64_FONT_SOURCE_OUTLINE,bytes,(size_t)n};
#endif
    }
}
static os64_font_status_t open_key(os64_font_family_cache_t *cache, unsigned key,
                                  os64_font_role_view_t *view)
{
    const os64_font_family_name_t names[] = {{"Georgia,",7}, {"Times New Roman,",15}};
    os64_font_family_list_t list = {names,2,(os64_font_family_t)(key%3)};
    unsigned style = key/3%4;
    return os64_font_family_open(cache,&list,style&1,style&2,12+key/12,view);
}
static os64_text_run_t *run(const os64_font_role_view_t *v)
{
    os64_text_layout_t layout = {.fonts=v->fonts,.font_count=v->font_count,
        .encoding=OS64_TEXT_UTF8_WESTERN_V1,.tab_interval=1024};
    os64_text_run_t *r = NULL;
    CHECK(!os64_text_layout(v->text,(const uint8_t *)"AV iW",5,&layout,&r));
    return r;
}
static size_t faces(os64_text_context_t *text)
{
    os64_font_engine_stats_t stats;
    CHECK(!text->backend->engine_stats(text->engine,&stats));
    CHECK(stats.live_faces <= OS64_FONT_FACE_MAX);
    return stats.live_faces;
}
static void normal(void)
{
    os64_text_context_t *text = context(128u*1024u*1024u);
    os64_font_family_cache_t *cache = NULL;
    CHECK(!os64_font_family_cache_create(text,specs,&cache,NULL,NULL));
    CHECK(os64_text_destroy(text)==OS64_FONT_BUSY);
    os64_font_role_view_t v;
    uint8_t *original=(uint8_t *)specs[0].styles[0].bytes;
    uint8_t saved=*original; *original=0;
    CHECK(!open_key(cache,0,&v)); uint64_t first = v.identity;
    *original=saved; /* opening uses the cache snapshot, not caller storage */
    os64_text_run_t *retained = run(&v);
    uint64_t second=0;
    for (unsigned k=1;k<32;++k) {CHECK(!open_key(cache,k,&v)); if (k==1) second=v.identity;}
    CHECK(faces(text)==32);
    CHECK(!open_key(cache,0,&v) && v.identity==first); /* touch first; key 1 now LRU */
    CHECK(!open_key(cache,32,&v)); CHECK(faces(text)==32);
    CHECK(!open_key(cache,1,&v) && v.identity!=second);
    CHECK(!open_key(cache,0,&v) && v.identity==first);
    for (unsigned k=33;k<96;++k) {CHECK(!open_key(cache,k,&v)); CHECK(faces(text)<=33);}
    CHECK(!open_key(cache,0,&v) && v.identity!=first);
    os64_text_run_view_t rv;
    CHECK(!os64_text_run_view(retained,&rv) && rv.glyphs[0].font_identity==first);
    CHECK(faces(text)==33);
    os64_text_run_release(retained); CHECK(faces(text)==32);
    retained=run(&v);
    CHECK(os64_font_family_open(cache,NULL,false,false,16,&v)==OS64_FONT_BAD_ARGUMENT);
    CHECK(!v.fonts && !v.font_count);
    os64_font_family_cache_destroy(cache);
    CHECK(faces(text)==1 && os64_text_destroy(text)==OS64_FONT_BUSY);
    CHECK(!os64_text_run_view(retained,&rv));
    os64_text_run_release(retained); CHECK(!os64_text_destroy(text)); CHECK(!live);
}
static void pinned_limit(void)
{
    os64_text_context_t *text=context(128u*1024u*1024u);
    os64_font_family_cache_t *cache=NULL;
    CHECK(!os64_font_family_cache_create(text,specs,&cache,NULL,NULL));
    os64_text_run_t *runs[64]; os64_font_role_view_t v;
    for (unsigned k=0;k<64;++k) {CHECK(!open_key(cache,k,&v)); runs[k]=run(&v);}
    CHECK(faces(text)==64);
    CHECK(open_key(cache,64,&v)==OS64_FONT_LIMIT && !v.fonts);
    for (unsigned k=0;k<64;++k) os64_text_run_release(runs[k]);
    CHECK(!open_key(cache,64,&v));
    os64_font_family_cache_destroy(cache); CHECK(!os64_text_destroy(text)); CHECK(!live);
}
static void fallbacks(void)
{
    os64_font_family_spec_t with[3]; memcpy(with,specs,sizeof(with));
    for (unsigned f=0;f<3;++f) {
        with[f].fallbacks[0]=specs[(f+1)%3].styles[0];
        with[f].fallbacks[1]=specs[(f+2)%3].styles[0];
        with[f].fallback_count=2;
    }
    os64_text_context_t *text=context(128u*1024u*1024u);
    os64_font_family_cache_t *cache=NULL;
    CHECK(!os64_font_family_cache_create(text,with,&cache,NULL,NULL));
    os64_font_role_view_t v;
    for (unsigned k=0;k<96;++k) {CHECK(!open_key(cache,k,&v)); CHECK(v.font_count==4); CHECK(faces(text)<=63);}
    os64_font_family_cache_destroy(cache); CHECK(!os64_text_destroy(text)); CHECK(!live);
}
#ifndef FAMILY_REAL
static void failures(void)
{
    for (size_t pos=1;pos<200;++pos) {
        os64_text_context_t *text=context(0); size_t start=calls;
        deny=start+pos;
        os64_font_family_cache_t *cache=NULL;
        os64_font_status_t status=os64_font_family_cache_create(text,specs,&cache,NULL,NULL);
        if (!status) {os64_font_role_view_t v; status=open_key(cache,0,&v);}
        bool hit=calls>=deny; deny=0;
        CHECK(status==OS64_FONT_OK || status==OS64_FONT_NO_MEMORY);
        os64_font_family_cache_destroy(cache); CHECK(!os64_text_destroy(text)); CHECK(!live);
        if (!hit) break;
        CHECK(pos<199);
    }
    /* A small byte cap must also cause eviction before the entry limit. */
    os64_text_context_t *text=context(24000);
    os64_font_family_cache_t *cache=NULL;
    CHECK(!os64_font_family_cache_create(text,specs,&cache,NULL,NULL));
    os64_font_role_view_t v;
    for (unsigned k=0;k<96;++k) {CHECK(!open_key(cache,k,&v)); CHECK(text->live<=text->cap);}
    CHECK(faces(text)<32);
    os64_font_family_cache_destroy(cache); CHECK(!os64_text_destroy(text)); CHECK(!live);
}
#endif
int main(int argc,char **argv)
{
    CHECK(argc>=2); sources(argv[1]);
    if (argc==3) {
        os64_text_context_t *text=context(0); os64_font_family_cache_t *cache=NULL;
        CHECK(!os64_font_family_cache_create(text,specs,&cache,NULL,NULL));
        os64_font_role_view_t v; CHECK(!open_key(cache,0,&v));
        os64_text_font_t *const *stale=v.fonts;
        CHECK(!open_key(cache,0,&v)); /* even a hit ends the borrowed list */
        os64_text_font_t *volatile invalid=stale[0]; (void)invalid;
        return 99;
    }
    normal(); pinned_limit(); fallbacks();
#ifndef FAMILY_REAL
    failures();
#endif
#ifdef FAMILY_REAL
    for (size_t f=0;f<3;++f) for (size_t s=0;s<4;++s) free((void *)specs[f].styles[s].bytes);
#endif
    printf("family cache: %zu checks, allocator peak %zu bytes, zero leaks\n",checks,peak);
    return 0;
}
