/* Guest check of the F2.5 provider/adoption boundary; the barrier is a refusal
 * fixture, not a PTY resize. F3 owns real terminal adoption evidence. */
#include "os64/os64.h"
#include "os64/font_provider.h"
#include "os64/font_adopt.h"
#include "os64/slurp.h"
#include "os64/mem.h"
#include "os64/str.h"

typedef union {max_align_t align;size_t bytes;} header;
static size_t live;
static void *allocate(void *user,size_t bytes)
{
    (void)user;header *p=os64_malloc(sizeof(*p)+bytes);
    if(!p)return NULL;
    p->bytes=bytes;live+=bytes;return p+1;
}
static void release(void *user,void *ptr,size_t bytes)
{
    (void)user;header *p=(header *)ptr-1;
    if(p->bytes!=bytes)os64_exit(3);
    live-=bytes;os64_free(p);
}
#define REQUIRE(x) do {if(!(x)){os64_printf("fontsettest: FAIL line %d\n",__LINE__);return 2;}}while(0)
typedef struct {os64_font_set_t *active,*pending;bool refuse;} state;
static os64_font_status_t prepare(void *user,os64_font_set_t *set,void **plan)
{
    state *s=user;*plan=NULL;os64_font_status_t result=os64_font_set_retain(set);
    if(result!=OS64_FONT_OK)return result;
    s->pending=set;*plan=s;return OS64_FONT_OK;
}
static os64_font_status_t barrier(void *user,void *plan)
{(void)plan;return ((state *)user)->refuse?OS64_FONT_ENGINE_ERROR:OS64_FONT_OK;}
static void commit(void *user,void *plan)
{(void)plan;state *s=user;os64_font_set_release(s->active);s->active=s->pending;s->pending=NULL;}
static void abort_plan(void *user,void *plan)
{(void)plan;state *s=user;os64_font_set_release(s->pending);s->pending=NULL;}
int main(void)
{
    os64_text_options_t options={.memory={NULL,allocate,release}};
    os64_text_context_t *text=NULL;REQUIRE(os64_font_context_create(&options,&text)==OS64_FONT_OK);
    state s={0};REQUIRE(os64_font_set_prepare(text,NULL,&s.active)==OS64_FONT_OK);
    os64_font_consumer_t consumer={&s,prepare,barrier,commit,abort_plan};
    const char *names[]={"DejaVuSans.ttf","DejaVuSansMono.ttf","SourceSans3-Regular.otf"};
    for(uint32_t size=12;size<=28;size+=16) {
        uint8_t *bytes[3]={0};size_t lengths[3]={0};os64_font_role_spec_t specs[3]={0};
        for(size_t n=0;n<3;n++) {
            char path[128];os64_snprintf(path,sizeof(path),"/tests/fonts/%s",names[n]);
            REQUIRE(os64_slurp(path,2u*1024u*1024u,&bytes[n],&lengths[n])==OS64_SLURP_OK);
            specs[n].pixel_height=size;specs[n].primary=(os64_font_source_t){OS64_FONT_SOURCE_OUTLINE,bytes[n],lengths[n]};
        }
        os64_font_set_t *candidate=NULL;
        REQUIRE(os64_font_set_prepare(text,specs,&candidate)==OS64_FONT_OK);
        for(size_t n=0;n<3;n++)os64_free(bytes[n]);
        os64_font_set_t *old=s.active;s.refuse=true;size_t failed=SIZE_MAX;
        REQUIRE(os64_font_adopt(candidate,&consumer,1,&failed)==OS64_FONT_ENGINE_ERROR && failed==0);
        REQUIRE(s.active==old && s.pending==NULL);s.refuse=false;
        REQUIRE(os64_font_adopt(candidate,&consumer,1,&failed)==OS64_FONT_OK);
        os64_font_set_release(candidate);
        os64_font_role_view_t terminal;REQUIRE(os64_font_set_view(s.active,OS64_FONT_ROLE_TERMINAL,&terminal)==OS64_FONT_OK);
        REQUIRE(terminal.font_count==2 && terminal.cell_width_px>0);
        os64_printf("fontsettest: %upx mono cell %dx%d baseline %d, refusal retained old set\n",size,terminal.cell_width_px,terminal.row_height_px,terminal.baseline_px);
    }
    REQUIRE(os64_text_destroy(text)==OS64_FONT_BUSY);
    os64_font_set_release(s.active);REQUIRE(os64_text_destroy(text)==OS64_FONT_OK && live==0);
    os64_printf("fontsettest: PASS, three roles, two sizes, zero live bytes\n");return 0;
}
