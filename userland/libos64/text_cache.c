#include "text_internal.h"
#include "os64/str.h"

typedef union { max_align_t align; size_t bytes; } allocation_header;
static void *allocate_raw(void *opaque, size_t bytes)
{
    os64_text_context_t *c=opaque;
    if (!bytes || bytes>SIZE_MAX-sizeof(allocation_header) ||
        bytes+sizeof(allocation_header)>c->cap-c->live) {
        c->refusal=OS64_FONT_LIMIT;return NULL;
    }
    size_t total=bytes+sizeof(allocation_header);
    allocation_header *p=c->memory.alloc(c->memory.context,total);
    if (!p) {c->refusal=OS64_FONT_NO_MEMORY;return NULL;}
    p->bytes=total;c->live+=total;
    return p+1;
}
void text_free(os64_text_context_t *c, void *p)
{
    if (!p) return;
    allocation_header *h=(allocation_header *)p-1;
    size_t bytes=h->bytes;
    c->live-=bytes;c->memory.free(c->memory.context,h,bytes);
}
static void backend_free(void *opaque, void *p, size_t bytes)
{
    (void)bytes;
    text_free(opaque,p);
}
os64_font_status_t text_status(os64_text_context_t *c, os64_font_status_t status)
{
    return status==OS64_FONT_NO_MEMORY && c->refusal==OS64_FONT_LIMIT
        ? OS64_FONT_LIMIT : status;
}
bool text_retry(os64_text_context_t *c, os64_font_status_t status)
{
    return text_status(c,status)==OS64_FONT_LIMIT && text_evict(c)!=0;
}
void *text_alloc(os64_text_context_t *c, size_t bytes)
{
    c->refusal=OS64_FONT_OK;
    void *p=allocate_raw(c,bytes);
    if (!p && text_retry(c,OS64_FONT_NO_MEMORY)) {
        c->refusal=OS64_FONT_OK;p=allocate_raw(c,bytes);
    }
    return p;
}
static bool valid_backend(const os64_font_backend_t *b)
{
    return b && b->revision==OS64_FONT_BACKEND_REVISION && b->struct_size==sizeof(*b) &&
        b->engine_create && b->engine_destroy && b->engine_stats && b->face_open &&
        b->face_close && b->face_info && b->lookup && b->pair_adjust && b->render &&
        b->glyph_view && b->glyph_release;
}
os64_font_status_t os64_text_create(const os64_text_options_t *o, os64_text_context_t **out)
{
    if (out) *out=NULL;
    if (!out || !o || !o->memory.alloc || !o->memory.free || !valid_backend(o->backend))
        return OS64_FONT_BAD_ARGUMENT;
    size_t cap=o->memory_cap?o->memory_cap:OS64_TEXT_MEMORY_DEFAULT;
    if (o->cache_cap>cap) return OS64_FONT_BAD_ARGUMENT;
    if (cap<sizeof(os64_text_context_t)) return OS64_FONT_LIMIT;
    os64_text_context_t *c=o->memory.alloc(o->memory.context,sizeof(*c));
    if (!c) return OS64_FONT_NO_MEMORY;
    *c=(os64_text_context_t){.memory=o->memory,.backend=o->backend,.cap=cap,
        .live=sizeof(*c),.cache_cap=o->cache_cap?o->cache_cap:OS64_TEXT_CACHE_DEFAULT,
        .next_identity=1};
    if (c->cache_cap>cap) c->cache_cap=cap;
    os64_font_engine_options_t engine_options={
        .memory={c,allocate_raw,backend_free},
        .memory_cap=cap<OS64_FONT_MEMORY_MAX?cap:OS64_FONT_MEMORY_MAX};
    os64_font_status_t status=c->backend->engine_create(&engine_options,&c->engine);
    if (status!=OS64_FONT_OK) {
        status=text_status(c,status);c->memory.free(c->memory.context,c,sizeof(*c));return status;
    }
    *out=c;return OS64_FONT_OK;
}
os64_font_status_t os64_text_destroy(os64_text_context_t *c)
{
    if (!c) return OS64_FONT_OK;
    if (c->fonts || c->runs) return OS64_FONT_BUSY;
    text_evict(c);
    os64_font_status_t status=c->backend->engine_destroy(c->engine);
    if (status!=OS64_FONT_OK) return status;
    os64_font_memory_t memory=c->memory;
    memory.free(memory.context,c,sizeof(*c));return OS64_FONT_OK;
}
static os64_font_status_t new_font(os64_text_context_t *c, os64_text_font_t **out)
{
    if (c->next_identity==UINT64_MAX) return OS64_FONT_LIMIT;
    os64_text_font_t *f=text_alloc(c,sizeof(*f));
    if (!f) return c->refusal;
    *f=(os64_text_font_t){.context=c,.identity=c->next_identity++,.refs=1};
    *out=f;return OS64_FONT_OK;
}
os64_font_status_t os64_text_font_open(os64_text_context_t *c, const uint8_t *bytes,
    size_t length, const os64_font_face_options_t *o, os64_text_font_t **out)
{
    if (out) *out=NULL;
    if (!c || !out || !bytes || !length || !o || !o->pixel_height ||
        o->pixel_height>OS64_FONT_PIXEL_MAX || (o->hint!=OS64_FONT_HINT_NORMAL && o->hint!=OS64_FONT_HINT_NONE))
        return OS64_FONT_BAD_ARGUMENT;
    if (length>OS64_FONT_FILE_MAX) return OS64_FONT_LIMIT;
    os64_text_font_t *f=NULL;
    os64_font_status_t status=new_font(c,&f);
    if (status!=OS64_FONT_OK) return status;
    f->options=*o;f->bytes=text_alloc(c,length);
    if (!f->bytes) {status=c->refusal;goto fail;}
    os64_memcpy(f->bytes,bytes,length);
    for (unsigned attempt=0;;attempt++) {
        c->refusal=OS64_FONT_OK;
        status=c->backend->face_open(c->engine,f->bytes,length,o,&f->face);
        if (!attempt && status!=OS64_FONT_OK && text_retry(c,status)) continue;
        status=text_status(c,status);break;
    }
    if (status!=OS64_FONT_OK) goto fail;
    status=c->backend->face_info(f->face,&f->info);
    if (status!=OS64_FONT_OK) goto fail;
    if (f->info.ascent<0 || f->info.descent<0 ||
        (int64_t)f->info.ascent+f->info.descent>f->info.line_height) {
        status=OS64_FONT_ENGINE_ERROR;goto fail;
    }
    f->next=c->fonts;c->fonts=f;*out=f;return OS64_FONT_OK;
fail:
    c->backend->face_close(f->face);text_free(c,f->bytes);text_free(c,f);return status;
}
os64_font_status_t os64_text_font_bitmap(os64_text_context_t *c, os64_text_font_t **out)
{
    if (out) *out=NULL;
    if (!c || !out) return OS64_FONT_BAD_ARGUMENT;
    os64_text_font_t *f=NULL;
    os64_font_status_t status=new_font(c,&f);
    if (status!=OS64_FONT_OK) return status;
    f->options.pixel_height=16;
    f->info=(os64_font_face_info_t){.glyph_count=256,.flags=OS64_FONT_FACE_FIXED_WIDTH,
        .family="Builtin",.style="8x16",.ascent=768,.descent=256,.line_height=1024};
    f->next=c->fonts;c->fonts=f;*out=f;return OS64_FONT_OK;
}
void os64_text_font_release(os64_text_font_t *f)
{
    if (!f || --f->refs) return;
    os64_text_context_t *c=f->context;
    text_drop_font_cache(c,f->identity);
    os64_text_font_t **at=&c->fonts;
    while (*at!=f) at=&(*at)->next;
    *at=f->next;c->backend->face_close(f->face);
    text_free(c,f->bytes);text_free(c,f);
}
static size_t bucket(uint64_t identity, uint32_t index)
{
    return (identity*UINT64_C(11400714819323198485)+index)%TEXT_BUCKETS;
}
static void unlink_lru(text_image *i)
{
    os64_text_context_t *c=i->context;
    if (i->older) i->older->newer=i->newer;else c->oldest=i->newer;
    if (i->newer) i->newer->older=i->older;else c->newest=i->older;
}
static void touch(text_image *i)
{
    os64_text_context_t *c=i->context;
    i->older=c->newest;i->newer=NULL;
    if (c->newest) c->newest->newer=i;else c->oldest=i;
    c->newest=i;
}
static void remove_image(text_image *i)
{
    os64_text_context_t *c=i->context;
    text_image **at=&c->buckets[bucket(i->identity,i->index)];
    while (*at!=i) at=&(*at)->hash_next;
    *at=i->hash_next;unlink_lru(i);c->cache_bytes-=i->cost;
    c->backend->glyph_release(i->glyph);text_free(c,i->owned);text_free(c,i);
}
size_t text_evict(os64_text_context_t *c)
{
    size_t before=c->live;
    for (text_image *i=c->oldest,*next;i;i=next) {
        next=i->newer;if (!i->pins) remove_image(i);
    }
    return before-c->live;
}
void text_trim(os64_text_context_t *c)
{
    for (text_image *i=c->oldest,*next;i && c->cache_bytes>c->cache_cap;i=next) {
        next=i->newer;if (!i->pins) remove_image(i);
    }
}
void text_drop_font_cache(os64_text_context_t *c, uint64_t identity)
{
    for (text_image *i=c->oldest,*next;i;i=next) {
        next=i->newer;if (i->identity==identity && !i->pins) remove_image(i);
    }
}
void text_image_unpin(text_image *i)
{
    if (i) i->pins--;
}
os64_font_status_t text_image_get(os64_text_font_t *f, uint32_t index,
    uint32_t width, uint32_t height, int32_t top, text_image **out)
{
    os64_text_context_t *c=f->context;
    uint64_t identity=index==TEXT_MARKER?0:f->identity;
    size_t hash=bucket(identity,index);
    *out=NULL;
    for (text_image *i=c->buckets[hash];i;i=i->hash_next)
        if (i->identity==identity && i->index==index && i->width==width &&
            i->height==height && i->top==top) {
            i->pins++;unlink_lru(i);touch(i);*out=i;return OS64_FONT_OK;
        }
    text_image *i=text_alloc(c,sizeof(*i));
    if (!i) return c->refusal;
    *i=(text_image){.context=c,.identity=identity,.index=index,.width=width,
        .height=height,.top=top,.pins=1};
    /* The cache target covers entry records and coverage. Opaque backend
     * bookkeeping and face scratch storage remain in the total context budget;
     * render-time growth is not necessarily storage evicting a glyph frees. */
    size_t entry_bytes=sizeof(*i)+sizeof(allocation_header);
    os64_font_status_t status=OS64_FONT_OK;
    if (f->face && !width) {
        for (unsigned attempt=0;;attempt++) {
            c->refusal=OS64_FONT_OK;
            status=c->backend->render(f->face,index,&i->glyph);
            if (!attempt && status!=OS64_FONT_OK && text_retry(c,status)) continue;
            status=text_status(c,status);break;
        }
        if (status!=OS64_FONT_OK) goto fail;

        status=c->backend->glyph_view(i->glyph,&i->view);
        if (status!=OS64_FONT_OK) goto fail;
        i->cost=entry_bytes+(size_t)i->view.width*i->view.height;
    } else {
        if (!width || !height || width>OS64_FONT_MASK_DIM_MAX || height>OS64_FONT_MASK_DIM_MAX) {
            status=OS64_FONT_LIMIT;goto fail;
        }
        size_t area=(size_t)width*height;
        i->owned=text_alloc(c,area);
        if (!i->owned) {status=c->refusal;goto fail;}
        i->cost=entry_bytes+area+sizeof(allocation_header);
        const uint8_t *rows=NULL;
        if (!f->face && width==8 && height==16 && index!=TEXT_MARKER) rows=text_bitmap_rows(index);
        bool ink=false;
        for (uint32_t y=0;y<height;y++) for (uint32_t x=0;x<width;x++) {
            uint8_t a;
            if (index==TEXT_MARKER) a=(x==0 || y==0 || x==width-1 || y==height-1)?255:0;
            else if (rows) a=(y<16 && x<8 && (rows[y]&(0x80u>>x)))?255:0;
            else a=text_box_pixel(index,x,y,width,height);
            i->owned[(size_t)y*width+x]=a;ink|=a!=0;
        }
        i->view.advance_x=(int32_t)width*64;
        if (ink) i->view=(os64_font_glyph_view_t){.advance_x=(int32_t)width*64,
            .width=width,.height=height,.stride=width,.top=top,.coverage=i->owned,
            .ink={0,top*64,(int32_t)width*64,(top+(int32_t)height)*64}};
    }
    if (i->view.advance_x<0 || i->view.width>OS64_FONT_MASK_DIM_MAX ||
        i->view.height>OS64_FONT_MASK_DIM_MAX || i->view.stride!=i->view.width ||
        ((i->view.width && i->view.height) && !i->view.coverage)) {
        status=OS64_FONT_ENGINE_ERROR;goto fail;
    }
    i->hash_next=c->buckets[hash];c->buckets[hash]=i;touch(i);
    c->cache_bytes+=i->cost;*out=i;text_trim(c);return OS64_FONT_OK;
fail:
    c->backend->glyph_release(i->glyph);text_free(c,i->owned);text_free(c,i);return status;
}
