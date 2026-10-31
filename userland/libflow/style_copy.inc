/* Deep copies of derived style lists. Memoizing source slices preserves
 * sharing within the new arena; no pointer into the old tree is retained. */
// Size changes require reviewing both the copy and equality inventories.
// This tripwire does not replace checking pointer fields in a same-size edit.
_Static_assert(sizeof(flow_style_t) == 1264, "review style copy/equality fields");
typedef struct { void *data; size_t bytes; } FCopySlice;
typedef struct { FArena *arena; FMap slices; size_t bytes; bool budget; } FStyleCopy;
static void *style_copy_bytes(FStyleCopy *c, const void *from, size_t bytes, bool *fresh)
{
    *fresh = false;
    if (!from || !bytes) return NULL;
    FCopySlice *had = f_map_get(&c->slices, from);
    if (had && had->bytes >= bytes) return had->data;
    if (bytes > F_STYLE_LISTS_BUDGET - c->bytes) { c->budget = true; return NULL; }
    FCopySlice *slice = f_arena_alloc(c->arena, sizeof(*slice));
    void *copy = slice ? f_arena_alloc(c->arena, bytes) : NULL;
    if (!copy) return NULL;
    os64_memcpy(copy, from, bytes);
    *slice = (FCopySlice){copy, bytes};
    if (!f_map_put(&c->slices, from, slice)) return NULL;
    c->bytes += bytes; *fresh = true;
    return copy;
}
static bool style_copy_string(FStyleCopy *c, const char **value, size_t len)
{
    if (!*value) return true;
    bool fresh;
    const char *copy = style_copy_bytes(c, *value, len ? len : 1, &fresh);
    if (!copy) return false;
    *value = copy; return true;
}
static bool style_copy_transforms(FStyleCopy *c, const flow_transform_t **value, int32_t n, int depth)
{
    if (!*value || !n) return true;
    if (depth > F_BLEND_DEPTH_MAX) return false;
    bool fresh;
    flow_transform_t *copy = style_copy_bytes(c, *value, (size_t)n * sizeof(*copy), &fresh);
    if (!copy) return false;
    *value = copy;
    if (fresh) for (int32_t i=0;i<n;++i) if (copy[i].blend) {
        bool new_blend;
        flow_transform_blend_t *b=style_copy_bytes(c,copy[i].blend,sizeof(*b),&new_blend);
        if(!b)return false;
        copy[i].blend=b;
        if(new_blend && (!style_copy_transforms(c,&b->from,b->nfrom,depth+1) ||
                         !style_copy_transforms(c,&b->to,b->nto,depth+1)))return false;
    }
    return true;
}
static bool style_copy_gradient(FStyleCopy *c, const flow_gradient_t **value)
{
    if(!*value)return true;
    bool fresh;
    flow_gradient_t *copy=style_copy_bytes(c,*value,sizeof(*copy),&fresh);
    if(!copy)return false;
    *value=copy;
    if(fresh && copy->nstops) {
        copy->stops=style_copy_bytes(c,copy->stops,(size_t)copy->nstops*sizeof(*copy->stops),&fresh);
        if(!copy->stops)return false;
    }
    return true;
}
static bool style_copy_value(FStyleCopy *c, flow_style_t *s)
{
    bool fresh;
#define COPY(field, count) do { \
    if(s->field && (count)) { \
        s->field=style_copy_bytes(c,s->field,(size_t)(count)*sizeof(*s->field),&fresh); \
        if(!s->field)return false; \
    } else fresh=false; \
} while(0)
    COPY(family.names,s->family.count);
    if(fresh)for(uint32_t i=0;i<s->family.count;i++) {
        flow_family_name_t *n=(flow_family_name_t *)&s->family.names[i];
        if(!style_copy_string(c,&n->name,n->len))return false;
    }
    COPY(family.written,s->family.nwritten);
    if(fresh)for(uint32_t i=0;i<s->family.nwritten;i++) {
        flow_family_written_t *n=(flow_family_written_t *)&s->family.written[i];
        if(!style_copy_string(c,&n->name,n->len))return false;
    }
    COPY(backgrounds.image,s->backgrounds.nimage);
    if(fresh)for(int32_t i=0;i<s->backgrounds.nimage;i++) {
        flow_bg_image_t *b=(flow_bg_image_t *)&s->backgrounds.image[i];
        if(!style_copy_string(c,&b->url,b->len) || !style_copy_gradient(c,&b->gradient))return false;
    }
    COPY(backgrounds.repeat,s->backgrounds.nrepeat);
    COPY(backgrounds.x,s->backgrounds.nx);
    COPY(backgrounds.y,s->backgrounds.ny);
    COPY(backgrounds.size,s->backgrounds.nsize);
    COPY(backgrounds.origin,s->backgrounds.norigin);
    COPY(backgrounds.clip,s->backgrounds.nclip);
    COPY(box_shadows,s->nbox_shadows);
    COPY(text_shadows,s->ntext_shadows);
    COPY(grid_template_columns.tracks,s->grid_template_columns.n);
    COPY(grid_template_rows.tracks,s->grid_template_rows.n);
    COPY(grid_auto_columns.tracks,s->grid_auto_columns.n);
    COPY(grid_auto_rows.tracks,s->grid_auto_rows.n);
    COPY(animations.duration,s->animations.nduration);
    COPY(animations.delay,s->animations.ndelay);
    COPY(animations.easing,s->animations.neasing);
    COPY(animations.iterations,s->animations.niterations);
    COPY(animations.direction,s->animations.ndirection);
    COPY(animations.fill,s->animations.nfill);
    COPY(animations.paused,s->animations.npaused);
    COPY(animations.name,s->animations.nname);
    if(fresh)for(int32_t i=0;i<s->animations.nname;i++) {
        flow_name_t *n=(flow_name_t *)&s->animations.name[i];
        if(!style_copy_string(c,&n->text,n->len))return false;
    }
    COPY(grid_areas,s->ngrid_areas);
    if(fresh)for(int32_t i=0;i<s->ngrid_areas;i++) {
        flow_grid_area_t *a=(flow_grid_area_t *)&s->grid_areas[i];
        if(!style_copy_string(c,&a->name,a->name_len))return false;
    }
    if(!style_copy_string(c,&s->grid_row_start.name,s->grid_row_start.name_len))return false;
    if(!style_copy_string(c,&s->grid_row_end.name,s->grid_row_end.name_len))return false;
    if(!style_copy_string(c,&s->grid_column_start.name,s->grid_column_start.name_len))return false;
    if(!style_copy_string(c,&s->grid_column_end.name,s->grid_column_end.name_len))return false;
    if(!style_copy_transforms(c,&s->transform,s->ntransform,0))return false;
    if(!style_copy_transforms(c,&s->translate,1,0))return false;
    if(!style_copy_transforms(c,&s->rotate,1,0))return false;
    if(!style_copy_transforms(c,&s->scale,1,0))return false;
#undef COPY
    return true;
}

// Equality ignores storage addresses, but includes the complete scalar style
// and list contents. Padding differences can conservatively refuse reuse.
static bool style_bytes_equal(const void *a,const void *b,size_t n)
{
    return n==0 || (a && b && !os64_memcmp(a,b,n));
}
static bool style_string_equal(const char *a,const char *b,size_t n)
{
    return (!a && !b) || (a && b && style_bytes_equal(a,b,n));
}
static bool style_transforms_equal(const flow_transform_t *a,const flow_transform_t *b,int32_t n,int depth)
{
    if(!a || !b)return a==b;
    if(depth>F_BLEND_DEPTH_MAX)return false;
    for(int32_t i=0;i<n;i++) {
        flow_transform_t x=a[i],y=b[i];x.blend=y.blend=NULL;
        if(os64_memcmp(&x,&y,sizeof(x)))return false;
        const flow_transform_blend_t *u=a[i].blend,*v=b[i].blend;
        if(!u || !v) { if(u!=v)return false; continue; }
        flow_transform_blend_t p=*u,q=*v;p.from=p.to=q.from=q.to=NULL;
        if(os64_memcmp(&p,&q,sizeof(p)) || !style_transforms_equal(u->from,v->from,u->nfrom,depth+1) ||
           !style_transforms_equal(u->to,v->to,u->nto,depth+1))return false;
    }
    return true;
}
static bool style_gradients_equal(const flow_gradient_t *a,const flow_gradient_t *b)
{
    if(!a || !b)return a==b;
    flow_gradient_t x=*a,y=*b;x.stops=y.stops=NULL;
    return !os64_memcmp(&x,&y,sizeof(x)) && style_bytes_equal(a->stops,b->stops,(size_t)a->nstops*sizeof(*a->stops));
}
bool f_style_equal(const flow_style_t *a,const flow_style_t *b)
{
    flow_style_t x=*a,y=*b;
    x.family.names=y.family.names=NULL;
    x.family.written=y.family.written=NULL;
    x.backgrounds.image=y.backgrounds.image=NULL;
    x.backgrounds.repeat=y.backgrounds.repeat=NULL;
    x.backgrounds.x=y.backgrounds.x=NULL;
    x.backgrounds.y=y.backgrounds.y=NULL;
    x.backgrounds.size=y.backgrounds.size=NULL;
    x.backgrounds.origin=y.backgrounds.origin=NULL;
    x.backgrounds.clip=y.backgrounds.clip=NULL;
    x.box_shadows=y.box_shadows=NULL;
    x.text_shadows=y.text_shadows=NULL;
    x.grid_template_columns.tracks=y.grid_template_columns.tracks=NULL;
    x.grid_template_rows.tracks=y.grid_template_rows.tracks=NULL;
    x.grid_auto_columns.tracks=y.grid_auto_columns.tracks=NULL;
    x.grid_auto_rows.tracks=y.grid_auto_rows.tracks=NULL;
    x.animations.duration=y.animations.duration=NULL;
    x.animations.delay=y.animations.delay=NULL;
    x.animations.easing=y.animations.easing=NULL;
    x.animations.iterations=y.animations.iterations=NULL;
    x.animations.direction=y.animations.direction=NULL;
    x.animations.fill=y.animations.fill=NULL;
    x.animations.paused=y.animations.paused=NULL;
    x.animations.name=y.animations.name=NULL;
    x.grid_areas=y.grid_areas=NULL;
    x.grid_row_start.name=y.grid_row_start.name=NULL;
    x.grid_row_end.name=y.grid_row_end.name=NULL;
    x.grid_column_start.name=y.grid_column_start.name=NULL;
    x.grid_column_end.name=y.grid_column_end.name=NULL;
    x.transform=y.transform=NULL;
    x.translate=y.translate=NULL;
    x.rotate=y.rotate=NULL;
    x.scale=y.scale=NULL;
    if(os64_memcmp(&x,&y,sizeof(x)))return false;
    if(!style_bytes_equal(a->backgrounds.repeat,b->backgrounds.repeat,(size_t)(a->backgrounds.nrepeat)*sizeof(*a->backgrounds.repeat)))return false;
    if(!style_bytes_equal(a->backgrounds.x,b->backgrounds.x,(size_t)(a->backgrounds.nx)*sizeof(*a->backgrounds.x)))return false;
    if(!style_bytes_equal(a->backgrounds.y,b->backgrounds.y,(size_t)(a->backgrounds.ny)*sizeof(*a->backgrounds.y)))return false;
    if(!style_bytes_equal(a->backgrounds.size,b->backgrounds.size,(size_t)(a->backgrounds.nsize)*sizeof(*a->backgrounds.size)))return false;
    if(!style_bytes_equal(a->backgrounds.origin,b->backgrounds.origin,(size_t)(a->backgrounds.norigin)*sizeof(*a->backgrounds.origin)))return false;
    if(!style_bytes_equal(a->backgrounds.clip,b->backgrounds.clip,(size_t)(a->backgrounds.nclip)*sizeof(*a->backgrounds.clip)))return false;
    if(!style_bytes_equal(a->box_shadows,b->box_shadows,(size_t)(a->nbox_shadows)*sizeof(*a->box_shadows)))return false;
    if(!style_bytes_equal(a->text_shadows,b->text_shadows,(size_t)(a->ntext_shadows)*sizeof(*a->text_shadows)))return false;
    if(!style_bytes_equal(a->grid_template_columns.tracks,b->grid_template_columns.tracks,(size_t)(a->grid_template_columns.n)*sizeof(*a->grid_template_columns.tracks)))return false;
    if(!style_bytes_equal(a->grid_template_rows.tracks,b->grid_template_rows.tracks,(size_t)(a->grid_template_rows.n)*sizeof(*a->grid_template_rows.tracks)))return false;
    if(!style_bytes_equal(a->grid_auto_columns.tracks,b->grid_auto_columns.tracks,(size_t)(a->grid_auto_columns.n)*sizeof(*a->grid_auto_columns.tracks)))return false;
    if(!style_bytes_equal(a->grid_auto_rows.tracks,b->grid_auto_rows.tracks,(size_t)(a->grid_auto_rows.n)*sizeof(*a->grid_auto_rows.tracks)))return false;
    if(!style_bytes_equal(a->animations.duration,b->animations.duration,(size_t)(a->animations.nduration)*sizeof(*a->animations.duration)))return false;
    if(!style_bytes_equal(a->animations.delay,b->animations.delay,(size_t)(a->animations.ndelay)*sizeof(*a->animations.delay)))return false;
    if(!style_bytes_equal(a->animations.easing,b->animations.easing,(size_t)(a->animations.neasing)*sizeof(*a->animations.easing)))return false;
    if(!style_bytes_equal(a->animations.iterations,b->animations.iterations,(size_t)(a->animations.niterations)*sizeof(*a->animations.iterations)))return false;
    if(!style_bytes_equal(a->animations.direction,b->animations.direction,(size_t)(a->animations.ndirection)*sizeof(*a->animations.direction)))return false;
    if(!style_bytes_equal(a->animations.fill,b->animations.fill,(size_t)(a->animations.nfill)*sizeof(*a->animations.fill)))return false;
    if(!style_bytes_equal(a->animations.paused,b->animations.paused,(size_t)(a->animations.npaused)*sizeof(*a->animations.paused)))return false;
    for(uint32_t i=0;i<a->family.count;i++) {
        flow_family_name_t u=a->family.names[i],v=b->family.names[i];
        if(u.len!=v.len || !style_string_equal(u.name,v.name,u.len))return false;
    }
    for(uint32_t i=0;i<a->family.nwritten;i++) {
        flow_family_written_t u=a->family.written[i],v=b->family.written[i];
        if(u.len!=v.len || u.keyword!=v.keyword || !style_string_equal(u.name,v.name,u.len))return false;
    }
    for(int32_t i=0;i<a->backgrounds.nimage;i++) {
        flow_bg_image_t u=a->backgrounds.image[i],v=b->backgrounds.image[i];
        if(u.len!=v.len || u.sheet!=v.sheet || !style_string_equal(u.url,v.url,u.len) ||
           !style_gradients_equal(u.gradient,v.gradient))return false;
    }
    for(int32_t i=0;i<a->animations.nname;i++) {
        flow_name_t u=a->animations.name[i],v=b->animations.name[i];
        if(u.len!=v.len || !style_string_equal(u.text,v.text,u.len))return false;
    }
    for(int32_t i=0;i<a->ngrid_areas;i++) {
        flow_grid_area_t u=a->grid_areas[i],v=b->grid_areas[i];
        if(u.name_len!=v.name_len || !style_string_equal(u.name,v.name,u.name_len))return false;
        u.name=v.name=NULL;if(os64_memcmp(&u,&v,sizeof(u)))return false;
    }
    if(!style_string_equal(a->grid_row_start.name,b->grid_row_start.name,a->grid_row_start.name_len))return false;
    if(!style_string_equal(a->grid_row_end.name,b->grid_row_end.name,a->grid_row_end.name_len))return false;
    if(!style_string_equal(a->grid_column_start.name,b->grid_column_start.name,a->grid_column_start.name_len))return false;
    if(!style_string_equal(a->grid_column_end.name,b->grid_column_end.name,a->grid_column_end.name_len))return false;
    if(!style_transforms_equal(a->transform,b->transform,a->ntransform,0))return false;
    if(!style_transforms_equal(a->translate,b->translate,1,0))return false;
    if(!style_transforms_equal(a->rotate,b->rotate,1,0))return false;
    if(!style_transforms_equal(a->scale,b->scale,1,0))return false;
    return true;
}
