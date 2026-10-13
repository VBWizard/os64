#include "test_directory_host_support.h"
#include "os64/browser.h"
#include "os64/work.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

struct os64_work_pool { int alive; };
static struct os64_work_pool pool;
typedef struct { os64_work_id_t id; os64_work_t work; void *product; int64_t verdict;
    bool occupied,running,done,cancelled; } slot_t;
static slot_t slots[64];static uint64_t serial;
static bool fail_restore;
static bool refuse_submit;
static size_t top;static int64_t left;
static bool cancelled(void *p) { return ((slot_t *)p)->cancelled; }
static void start(size_t at) { assert(slots[at].occupied);slots[at].running=true; }
static void complete(size_t at)
{ slot_t *s=&slots[at];assert(s->occupied && !s->done);
  s->verdict=s->work.run(s->work.job,cancelled,s,&s->product);
  if (s->cancelled) {s->work.release(s->work.job,s->product);s->occupied=false;}
  else s->done=true; }
static size_t latest(void)
{ size_t at=0;uint64_t id=0;for (size_t i=0;i<64;++i)
    if (slots[i].occupied && slots[i].id>id) {id=slots[i].id;at=i;}
  assert(id);return at; }
os64_work_pool_t *os64_work_pool_create(int32_t workers,size_t cap,int64_t win,uint32_t bit)
{ assert(workers==2 && cap>0 && win && bit);pool.alive=1;return &pool; }
os64_work_id_t os64_work_submit(os64_work_pool_t *p,const os64_work_t *w)
{ assert(p==&pool);if (refuse_submit) return 0;
  for (size_t i=0;i<64;++i) if (!slots[i].occupied) {
    slots[i]=(slot_t){.id=++serial,.work=*w,.occupied=true};return serial; }return 0; }
void os64_work_cancel(os64_work_pool_t *p,os64_work_id_t id)
{ assert(p==&pool);for (size_t i=0;i<64;++i) if (slots[i].occupied && slots[i].id==id) {
    slots[i].cancelled=true;
    if (!slots[i].running) {slots[i].work.release(slots[i].work.job,slots[i].product);slots[i].occupied=false;}
    return; } }
bool os64_work_reap(os64_work_pool_t *p,os64_work_id_t *id,int64_t *verdict,void **job,void **product)
{ assert(p==&pool);size_t chosen=64;uint64_t oldest=UINT64_MAX;
  for (size_t i=0;i<64;++i) if (slots[i].occupied&&slots[i].done&&slots[i].id<oldest)
    {chosen=i;oldest=slots[i].id;}
  if (chosen==64) return false;
  slot_t *s=&slots[chosen];*id=s->id;*verdict=s->verdict;*job=s->work.job;*product=s->product;
  s->occupied=false;return true; }
int64_t os64_work_pool_error(os64_work_pool_t *p) { assert(p==&pool);return 0; }
void os64_work_pool_destroy(os64_work_pool_t *p)
{ assert(p==&pool);for (size_t i=0;i<64;++i) if (slots[i].occupied) {
    slots[i].work.release(slots[i].work.job,slots[i].product);slots[i].occupied=false; }
  pool.alive=0; }

os64_ui_status_code_t os64_ui_dataview_attach(os64_ui_dataview_t *v,
    os64_ui_selection_t *selection,const os64_ui_icon_provider_t *provider)
{ (void)provider;v->state=selection;return OS64_UI_OK; }
os64_ui_status_code_t os64_ui_dataview_dispose(os64_ui_dataview_t *v)
{ v->state=NULL;return OS64_UI_OK; }
os64_ui_status_code_t os64_ui_dataview_restore(os64_ui_dataview_t *v,
    const os64_ui_selection_t *saved,size_t new_top,int64_t new_left)
{ if (fail_restore) return OS64_UI_NO_MEMORY;
  os64_ui_selection_t tmp={0};os64_ui_status_code_t rc=os64_ui_selection_clone(saved,&tmp);
  if (rc) return rc;
  os64_ui_selection_t *live=v->state;os64_ui_selection_dispose(live);*live=tmp;
  top=new_top;left=new_left;return OS64_UI_OK; }
size_t os64_ui_dataview_top(const os64_ui_dataview_t *v) { (void)v;return top; }
int64_t os64_ui_dataview_left(const os64_ui_dataview_t *v) { (void)v;return left; }

static os64_dirent_t entries[3];
static void setup(const char *a,const char *b)
{ memset(entries,0,sizeof(entries));strcpy(entries[0].name,a);
  strcpy(entries[1].name,b);test_entries=entries;test_entry_count=2;test_reset_io(); }
static void finish(os64_browser_t *b)
{ size_t i=latest();complete(i);assert(os64_browser_poll(b)==OS64_UI_OK); }
int main(void)
{
    os64_ui_dataview_t view={0};os64_browser_t *b=NULL;
    assert(os64_browser_create(&view,NULL,22,1,&b)==OS64_UI_OK);
    setup("alpha","beta");
    assert(os64_browser_navigate(b,"/A")==OS64_UI_OK);
    size_t a=latest();start(a);
    assert(os64_browser_navigate(b,"/B")==OS64_UI_OK);
    size_t bb=latest();assert(bb!=a);
    complete(bb);assert(os64_browser_poll(b)==OS64_UI_OK);
    assert(!strcmp(os64_browser_location(b),"/B"));
    complete(a);assert(os64_browser_poll(b)==OS64_UI_OK);
    assert(!strcmp(os64_browser_location(b),"/B"));
    refuse_submit=true;
    assert(os64_browser_navigate(b,"/refused")==OS64_UI_REFUSED);
    assert(!strcmp(os64_browser_location(b),"/B") && !os64_browser_loading(b));
    refuse_submit=false;
    uint64_t old_domain=os64_directory_domain(os64_browser_directory(b));
    assert(os64_browser_filter(b,"ALPHA",false)==OS64_UI_OK);
    assert(os64_ui_data_projection_count(os64_ui_selection_projection((os64_ui_selection_t *)view.state))==1);
    assert(os64_browser_refresh(b)==OS64_UI_OK);
    assert(os64_browser_filter(b,"beta",false)==OS64_UI_OK);
    finish(b);
    assert(!strcmp(os64_browser_filter_text(b),"beta"));
    assert(os64_ui_data_projection_count(os64_ui_selection_projection((os64_ui_selection_t *)view.state))==1);
    assert(os64_directory_domain(os64_browser_directory(b))==old_domain);
    assert(os64_browser_navigate(b,"/C")==OS64_UI_OK);
    fail_restore=true;complete(latest());
    assert(os64_browser_poll(b)==OS64_UI_NO_MEMORY);
    assert(!strcmp(os64_browser_location(b),"/B") && !os64_browser_can_back(b));
    fail_restore=false;
    assert(os64_browser_navigate(b,"/C")==OS64_UI_OK);finish(b);
    assert(os64_browser_can_back(b));
    assert(os64_browser_back(b)==OS64_UI_OK);
    fail_restore=true;complete(latest());
    assert(os64_browser_poll(b)==OS64_UI_NO_MEMORY);
    assert(!strcmp(os64_browser_location(b),"/C"));
    fail_restore=false;
    assert(os64_browser_back(b)==OS64_UI_OK);finish(b);
    assert(!strcmp(os64_browser_location(b),"/B"));
    assert(os64_browser_can_forward(b));
    assert(os64_browser_activate(b,999,1)==OS64_UI_STALE);
    assert(os64_browser_activate(b,os64_directory_domain(os64_browser_directory(b)),1)==OS64_UI_REFUSED);
    uint64_t cached_domain=os64_directory_domain(os64_browser_directory(b));
    for (int i=0;i<9;++i) {
        char path[32];snprintf(path,sizeof(path),"/evict/%d",i);
        assert(os64_browser_navigate(b,path)==OS64_UI_OK);finish(b);
    }
    for (int i=0;i<9;++i) {assert(os64_browser_back(b)==OS64_UI_OK);finish(b);}
    assert(!strcmp(os64_browser_location(b),"/B"));
    assert(os64_directory_domain(os64_browser_directory(b))!=cached_domain);
    test_read_error_at=0;
    assert(os64_browser_navigate(b,"/bad")==OS64_UI_OK);
    complete(latest());assert(os64_browser_poll(b)==OS64_UI_ERROR);
    assert(!strcmp(os64_browser_location(b),"/B"));
    test_reset_io();
    assert(os64_browser_navigate(b,"/cancelled")==OS64_UI_OK);
    size_t cancelled_slot=latest();start(cancelled_slot);
    os64_browser_cancel(b);complete(cancelled_slot);os64_browser_poll(b);
    assert(!strcmp(os64_browser_location(b),"/B") && !os64_browser_loading(b));
    assert(os64_browser_navigate(b,"/X")==OS64_UI_OK);
    size_t x=latest();start(x);
    assert(os64_browser_navigate(b,"/Y")==OS64_UI_OK);
    size_t y=latest();start(y);
    assert(os64_browser_navigate(b,"/Z")==OS64_UI_OK);
    assert(os64_browser_loading(b) && !strcmp(os64_browser_requested(b),"/Z"));
    complete(y);assert(os64_browser_poll(b)==OS64_UI_OK);
    size_t z=latest();assert(z!=x);
    complete(z);assert(os64_browser_poll(b)==OS64_UI_OK);
    complete(x);os64_browser_poll(b);
    assert(!strcmp(os64_browser_location(b),"/Z"));
    for (int i=0;i<35;++i) {
        char path[32];snprintf(path,sizeof(path),"/many/%d",i);
        assert(os64_browser_navigate(b,path)==OS64_UI_OK);finish(b);
    }
    assert(os64_browser_can_back(b));
    assert(os64_browser_navigate(b,"/unfinished")==OS64_UI_OK);
    start(latest());os64_browser_destroy(b);
    assert(!pool.alive);
    puts("browser host: ok");return 0;
}
