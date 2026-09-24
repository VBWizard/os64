#include "os64/font_provider.h"
#include "os64/font_adopt.h"
#include "fake_backend.h"
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#ifndef PROVIDER_REAL
/* The fake-only binary has no production engine; --real links the F1 getter. */
const os64_font_backend_t *os64_freetype_backend_v1(void)
{return os64_fake_font_backend();}
#endif
static size_t checks,live,calls,deny;
#define CHECK(x) do { checks++; if (!(x)) {fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);exit(1);} } while (0)
typedef union {max_align_t align;size_t bytes;} header;
static void *allocate(void *user,size_t bytes)
{
    (void)user;
    if (++calls==deny) return NULL;
    header *p=malloc(sizeof(*p)+bytes);
    CHECK(p!=NULL);p->bytes=bytes;live+=bytes;return p+1;
}
static void release(void *user,void *ptr,size_t bytes)
{
    (void)user;header *p=(header *)ptr-1;
    CHECK(p->bytes==bytes && live>=bytes);live-=bytes;free(p);
}
static os64_text_context_t *context(const os64_font_backend_t *backend)
{
    os64_text_options_t options={.memory={NULL,allocate,release},.backend=backend};
    os64_text_context_t *c=NULL;CHECK(os64_font_context_create(&options,&c)==OS64_FONT_OK);return c;
}
static void finish(os64_text_context_t *c)
{CHECK(os64_text_destroy(c)==OS64_FONT_OK);CHECK(live==0);}
static os64_font_source_t source(uint8_t *bytes,size_t length)
{return (os64_font_source_t){OS64_FONT_SOURCE_OUTLINE,bytes,length};}
static os64_text_run_t *run(os64_font_set_t *set,os64_font_role_t role,const char *bytes)
{
    os64_font_role_view_t v;CHECK(os64_font_set_view(set,role,&v)==OS64_FONT_OK);
    os64_text_layout_t options={.fonts=v.fonts,.font_count=v.font_count,.tab_interval=512};
    os64_text_run_t *result=NULL;
    CHECK(os64_text_layout(v.text,(const uint8_t *)bytes,strlen(bytes),&options,&result)==OS64_FONT_OK);
    return result;
}
static void snapshots(void)
{
    os64_text_context_t *c=context(os64_fake_font_backend());os64_font_set_t *a=NULL,*b=NULL;
    CHECK(os64_font_set_prepare(c,NULL,&a)==OS64_FONT_OK);
    for(int role=0;role<OS64_FONT_ROLE_COUNT;role++) {
        os64_font_role_view_t v;CHECK(os64_font_set_view(a,role,&v)==OS64_FONT_OK);
        CHECK(v.text==c && v.font_count==1 && v.baseline_px==12 && v.row_height_px==16);
        CHECK(v.cell_width_px==(role==OS64_FONT_ROLE_TERMINAL?8:0));
    }
    uint8_t p='P',l='L';os64_font_role_spec_t specs[3]={0};
    specs[0].pixel_height=12;specs[0].primary=source(&p,1);
    specs[0].fallback_count=1;specs[0].fallbacks[0]=source(&l,1);
    specs[2].pixel_height=28;specs[2].primary=source(&p,1);
    CHECK(os64_font_set_prepare(c,specs,&b)==OS64_FONT_OK);
    p='M';l='M';
    os64_font_role_view_t av,bv;CHECK(os64_font_set_view(a,0,&av)==OS64_FONT_OK);
    CHECK(os64_font_set_view(b,0,&bv)==OS64_FONT_OK);
    CHECK(av.identity!=bv.identity && bv.font_count==3);
    os64_text_run_t *r=run(b,0,"iW");os64_text_run_view_t rv;
    CHECK(os64_text_run_view(r,&rv)==OS64_FONT_OK && rv.advance_x==768);
    CHECK(os64_text_destroy(c)==OS64_FONT_BUSY);
    CHECK(os64_font_set_retain(b)==OS64_FONT_OK);os64_font_set_release(b);
    os64_font_set_release(a);os64_font_set_release(b);
    CHECK(os64_text_destroy(c)==OS64_FONT_BUSY);
    CHECK(os64_text_run_view(r,&rv)==OS64_FONT_OK && rv.advance_x==768);
    os64_text_run_release(r);finish(c);
}
static os64_font_status_t all_ascii(os64_font_face_t *f,uint32_t scalar,uint32_t *out)
{
    return os64_fake_font_backend()->lookup(f,scalar>=32 && scalar<=126?'W':scalar,out);
}
static os64_font_status_t fractional_info(os64_font_face_t *f,os64_font_face_info_t *out)
{
    os64_font_status_t status=os64_fake_font_backend()->face_info(f,out);
    if(status==OS64_FONT_OK){out->ascent=641;out->descent=63;out->line_height=769;}
    return status;
}
static os64_font_status_t fixed_info(os64_font_face_t *f,os64_font_face_info_t *out)
{
    os64_font_status_t status=os64_fake_font_backend()->face_info(f,out);
    if(status==OS64_FONT_OK)out->flags|=OS64_FONT_FACE_FIXED_WIDTH;
    return status;
}
static os64_font_status_t varying_ascii(os64_font_face_t *f,uint32_t scalar,uint32_t *out)
{return os64_fake_font_backend()->lookup(f,scalar=='i'?'i':'W',out);}
static os64_font_status_t fractional_advance(os64_font_glyph_t *g,os64_font_glyph_view_t *out)
{
    os64_font_status_t status=os64_fake_font_backend()->glyph_view(g,out);
    if(status==OS64_FONT_OK)out->advance_x++;
    return status;
}
static void suitability(void)
{
    os64_text_context_t *c=context(os64_fake_font_backend());os64_font_set_t *set=NULL;
    uint8_t m='M',p='P';os64_font_role_spec_t specs[3]={0};
    specs[1].primary=source(&p,1);os64_font_problem_t problem;
    CHECK(os64_font_set_prepare_checked(c,specs,&set,&problem)==OS64_FONT_UNSUPPORTED && !set);
    CHECK(problem.role==OS64_FONT_ROLE_TERMINAL && problem.source_index==0);
    specs[0].fallback_count=1;specs[0].fallbacks[0]=(os64_font_source_t){OS64_FONT_SOURCE_OUTLINE,NULL,1};
    CHECK(os64_font_set_prepare_checked(c,specs,&set,&problem)==OS64_FONT_BAD_ARGUMENT && !set);
    CHECK(problem.role==OS64_FONT_ROLE_UI && problem.source_index==1);
    specs[0]=(os64_font_role_spec_t){0};
    specs[1].primary=source(&m,1);
    CHECK(os64_font_set_prepare(c,specs,&set)==OS64_FONT_UNSUPPORTED && !set); /* incomplete ASCII */
    specs[1]=(os64_font_role_spec_t){.pixel_height=12};
    CHECK(os64_font_set_prepare(c,specs,&set)==OS64_FONT_BAD_ARGUMENT && !set);
    specs[1]=(os64_font_role_spec_t){.fallback_count=1};
    CHECK(os64_font_set_prepare(c,specs,&set)==OS64_FONT_BAD_ARGUMENT && !set); /* repeated builtin */
    finish(c);
    os64_font_backend_t backend=*os64_fake_font_backend();backend.lookup=all_ascii;
    c=context(&backend);specs[1]=(os64_font_role_spec_t){.primary=source(&m,1)};
    CHECK(os64_font_set_prepare(c,specs,&set)==OS64_FONT_OK);
    os64_font_role_view_t v;CHECK(os64_font_set_view(set,1,&v)==OS64_FONT_OK);
    CHECK(v.cell_width_px==8 && v.font_count==2);
    os64_font_set_release(set);finish(c);
    backend.face_info=fixed_info;backend.lookup=varying_ascii;
    c=context(&backend);specs[1].primary=source(&p,1);
    CHECK(os64_font_set_prepare(c,specs,&set)==OS64_FONT_UNSUPPORTED && !set);finish(c);
    backend=*os64_fake_font_backend();backend.lookup=all_ascii;backend.glyph_view=fractional_advance;
    c=context(&backend);specs[1].primary=source(&m,1);
    CHECK(os64_font_set_prepare(c,specs,&set)==OS64_FONT_UNSUPPORTED && !set);finish(c);
    backend=*os64_fake_font_backend();
    backend.face_info=fractional_info;c=context(&backend);
    specs[0]=(os64_font_role_spec_t){.primary=source(&p,1)};
    specs[1]=(os64_font_role_spec_t){0};
    CHECK(os64_font_set_prepare(c,specs,&set)==OS64_FONT_OK);
    CHECK(os64_font_set_view(set,0,&v)==OS64_FONT_OK && v.baseline_px==11 && v.row_height_px==13);
    os64_font_set_release(set);finish(c);
}
static void failure_cleanup(void)
{
    size_t count=0;
    for(size_t fail=0;!fail || fail<=count;fail++) {
        os64_text_context_t *c=context(os64_fake_font_backend());
        os64_font_set_t *old=NULL,*fresh=NULL;
        CHECK(os64_font_set_prepare(c,NULL,&old)==OS64_FONT_OK);
        uint8_t p='P';os64_font_role_spec_t specs[3]={0};specs[0].primary=source(&p,1);
        size_t start=calls;deny=fail?start+fail:0;
        os64_font_status_t status=os64_font_set_prepare(c,specs,&fresh);deny=0;
        if(!fail){CHECK(status==OS64_FONT_OK);count=calls-start;}
        else CHECK(status==OS64_FONT_NO_MEMORY && !fresh);
        os64_font_role_view_t v;CHECK(os64_font_set_view(old,1,&v)==OS64_FONT_OK && v.cell_width_px==8);
        os64_font_set_release(fresh);os64_font_set_release(old);finish(c);
    }
    printf("allocation-denial points: %zu\n",count);
}

typedef struct {int id;bool fail_prepare,fail_barrier;os64_font_set_t *active,*pending;} consumer;
static int events[64];static size_t event_count;
static os64_font_status_t prepare(void *user,os64_font_set_t *set,void **out)
{
    consumer *c=user;events[event_count++]=10+c->id;*out=NULL;
    if(c->fail_prepare)return OS64_FONT_NO_MEMORY;
    os64_font_status_t status=os64_font_set_retain(set);
    if(status!=OS64_FONT_OK)return status;
    c->pending=set;*out=c;return OS64_FONT_OK;
}
static os64_font_status_t barrier(void *user,void *plan)
{
    consumer *c=user;CHECK(plan==c);events[event_count++]=20+c->id;
    return c->fail_barrier?OS64_FONT_ENGINE_ERROR:OS64_FONT_OK;
}
static void commit(void *user,void *plan)
{
    consumer *c=user;CHECK(plan==c);events[event_count++]=30+c->id;
    os64_font_set_release(c->active);c->active=c->pending;c->pending=NULL;
}
static void abort_plan(void *user,void *plan)
{
    consumer *c=user;CHECK(plan==c);events[event_count++]=40+c->id;
    os64_font_set_release(c->pending);c->pending=NULL;
}
static void transaction(void)
{
    os64_text_context_t *c=context(os64_fake_font_backend());os64_font_set_t *set=NULL,*old=NULL;
    CHECK(os64_font_set_prepare(c,NULL,&old)==OS64_FONT_OK);
    CHECK(os64_font_set_prepare(c,NULL,&set)==OS64_FONT_OK);
    consumer clients[3]={{.id=0},{.id=1},{.id=2}};os64_font_consumer_t ops[3];
    for(size_t n=0;n<3;n++) {
        CHECK(os64_font_set_retain(old)==OS64_FONT_OK);clients[n].active=old;
        ops[n]=(os64_font_consumer_t){&clients[n],prepare,n==1?barrier:NULL,commit,abort_plan};
    }
    size_t failed=0;clients[1].fail_prepare=true;event_count=0;
    CHECK(os64_font_adopt(set,ops,3,&failed)==OS64_FONT_NO_MEMORY && failed==1);
    const int prepare_fail[]={10,11,40};CHECK(event_count==3 && !memcmp(events,prepare_fail,sizeof(prepare_fail)));
    for(size_t n=0;n<3;n++)CHECK(clients[n].active==old && !clients[n].pending);
    clients[1].fail_prepare=false;clients[1].fail_barrier=true;event_count=0;
    CHECK(os64_font_adopt(set,ops,3,&failed)==OS64_FONT_ENGINE_ERROR && failed==1);
    const int barrier_fail[]={10,11,12,21,42,41,40};CHECK(event_count==7 && !memcmp(events,barrier_fail,sizeof(barrier_fail)));
    for(size_t n=0;n<3;n++)CHECK(clients[n].active==old && !clients[n].pending);
    ops[0].barrier=barrier;event_count=0;
    CHECK(os64_font_adopt(set,ops,3,&failed)==OS64_FONT_BAD_ARGUMENT && failed==SIZE_MAX && !event_count);
    ops[0].barrier=NULL;clients[1].fail_barrier=false;event_count=0;
    CHECK(os64_font_adopt(set,ops,3,&failed)==OS64_FONT_OK && failed==SIZE_MAX);
    const int success[]={10,11,12,21,30,31,32};CHECK(event_count==7 && !memcmp(events,success,sizeof(success)));
    os64_font_set_release(old);os64_font_set_release(set);
    for(size_t n=0;n<3;n++){CHECK(clients[n].active && !clients[n].pending);os64_font_set_release(clients[n].active);}
    finish(c);
}
#ifdef PROVIDER_REAL
static void real(const char *root)
{
    const char *names[]={"DejaVuSansMono.ttf","SourceCodePro-Regular.otf"};
    for(size_t n=0;n<2;n++)for(uint32_t size=12;size<=28;size+=16) {
        char path[1024];snprintf(path,sizeof(path),"%s/%s",root,names[n]);FILE *file=fopen(path,"rb");CHECK(file!=NULL);
        CHECK(fseek(file,0,SEEK_END)==0);long len=ftell(file);CHECK(len>0);rewind(file);
        uint8_t *bytes=malloc((size_t)len);CHECK(bytes!=NULL);CHECK(fread(bytes,1,(size_t)len,file)==(size_t)len);fclose(file);
        os64_font_role_spec_t specs[3]={0};specs[1].pixel_height=size;specs[1].primary=source(bytes,(size_t)len);
        os64_text_context_t *c=context(NULL);os64_font_set_t *set=NULL;
        CHECK(os64_font_set_prepare(c,specs,&set)==OS64_FONT_OK);free(bytes);
        os64_font_role_view_t v;CHECK(os64_font_set_view(set,1,&v)==OS64_FONT_OK);
        CHECK(v.cell_width_px>0 && v.row_height_px>0 && v.font_count==2);
        printf("real %s %upx: cell %dx%d baseline %d\n",names[n],size,v.cell_width_px,v.row_height_px,v.baseline_px);
        os64_font_set_release(set);finish(c);
    }
}
#endif
int main(int argc,char **argv)
{
    (void)argc;(void)argv;snapshots();suitability();failure_cleanup();transaction();
#ifdef PROVIDER_REAL
    CHECK(argc==2);real(argv[1]);
#endif
    printf("PASS: %zu provider/adoption checks\n",checks);return 0;
}
