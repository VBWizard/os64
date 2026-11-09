#include "page/image_source.h"

// Desired selection and completed selection are separate: a getter may see a
// mutation before the task boundary submits it, and an obsolete job can finish
// afterward. Catalog indices own immutable products for the page's lifetime;
// only promotion changes the product and density exposed by an element.
struct ImageSource {
    ImageSource *next;
    const os64_html_node_t *node;
    char *url;
    uint64_t version, base_hash, generation, current_generation;
    double density, current_density;
    garb_env_t environment;
    unsigned mode;                 // 1 no-CORS; 2 anonymous; 3 use-credentials
    int32_t display, native, current_display, current_native;
    bool wants_display, wants_native, empty, broken, refused;
};

static const char *image_attr(const os64_html_node_t *n, const char *name)
{
    const os64_html_attr_t *a=os64_html_attr(n,name);
    return a?a->value:"";
}

static bool image_type_supported(const char *type)
{
    if(!*type)return true;
    static const char *const types[]={"image/png","image/jpeg","image/pjpeg","image/gif","image/bmp",
        "image/x-ms-bmp","image/x-portable-pixmap","image/webp","image/svg+xml"};
    while(*type==' '||*type=='\t')type++;
    size_t n=0;while(type[n] && type[n]!=';' && type[n]!=' ' && type[n]!='\t')n++;
    for(size_t i=0;i<sizeof(types)/sizeof(*types);i++)
        if(n==os64_strlen(types[i])) {
            size_t k=0;for(;k<n;k++) {char c=type[k];if(c>='A'&&c<='Z')c+='a'-'A';if(c!=types[i][k])break;}
            if(k==n)return true;
        }
    return false;
}

static bool image_same_environment(garb_env_t a, garb_env_t b)
{
    return a.width==b.width && a.height==b.height && a.resolution==b.resolution &&
        a.device_width==b.device_width && a.device_height==b.device_height &&
        a.dark==b.dark && a.reduced_motion==b.reduced_motion && a.scripting==b.scripting;
}

static os64_page_source_status_t image_choose(const Page *p, const os64_html_node_t *node,
    os64_page_source_t *chosen)
{
    garb_env_t env=p->image_environment;
    if(env.width<=0) env=css_view(g.view.bounds.w,g.view.bounds.h,g.zoom?g.zoom:1000);
    env.scripting=p->scripting;
    const os64_html_node_t *parent=node->parent;
    if(node->tag==OS64_HTML_TAG_IMG && parent && parent->ns==OS64_HTML_NS_HTML && parent->tag==OS64_HTML_TAG_PICTURE) {
        unsigned count=0;
        for(const os64_html_node_t *s=parent->first_child;s && s!=node;s=s->next) {
            if(++count>4096)return OS64_PAGE_SOURCE_LIMIT;
            if(s->ns!=OS64_HTML_NS_HTML || s->tag!=OS64_HTML_TAG_SOURCE)continue;
            const char *media=image_attr(s,"media"), *sizes=image_attr(s,"sizes");
            if(os64_strlen(media)>65536 || os64_strlen(sizes)>65536)return OS64_PAGE_SOURCE_LIMIT;
            if(!image_type_supported(image_attr(s,"type")) || !garb_media_text_matches(media,env))continue;
            os64_page_source_status_t status=os64_page_source_select(image_attr(s,"srcset"),NULL,
                garb_image_source_size(sizes,env),env.resolution,chosen);
            if(status!=OS64_PAGE_SOURCE_EMPTY)return status;
        }
    }
    const char *sizes=image_attr(node,"sizes");
    if(os64_strlen(sizes)>65536)return OS64_PAGE_SOURCE_LIMIT;
    return os64_page_source_select(node->tag==OS64_HTML_TAG_IMG?image_attr(node,"srcset"):"",
        image_attr(node,"src"),garb_image_source_size(sizes,env),env.resolution,chosen);
}

static void image_source_read(Page *p, ImageSource *s)
{
    const os64_html_document_t *doc=page_doc(p);
    uint64_t version=os64_html_version(doc),hash=1469598103934665603ull;
    const char *base=p->way.url;
    for(const char *c=base;*c;c++){hash^=(unsigned char)*c;hash*=1099511628211ull;}
    garb_env_t env=p->image_environment;
    if(env.width<=0)env=css_view(g.view.bounds.w,g.view.bounds.h,g.zoom?g.zoom:1000);
    env.scripting=p->scripting;
    if(s->version==version && s->base_hash==hash && image_same_environment(s->environment,env))return;
    s->version=version;s->base_hash=hash;s->environment=env;
    os64_page_source_t chosen={0};
    os64_page_source_status_t status=image_choose(p,s->node,&chosen);
    char *url=NULL;bool invalid_url=false;
    if(status==OS64_PAGE_SOURCE_OK) {
        size_t old=s->url?os64_strlen(s->url)+1:0;
        if(chosen.length>=4u*1024u*1024u-p->image_source_bytes+old)status=OS64_PAGE_SOURCE_LIMIT;
        else {
            char *raw=os64_malloc(chosen.length+1);
            if(raw) {os64_memcpy(raw,chosen.url,chosen.length);raw[chosen.length]=0;}
            if(raw && data_address(raw,chosen.length)) {os64_memcpy(raw,"data:",5);url=raw;}
            else if(raw) {
                char resolved[OS64_FETCH_URL_MAX];
                if(os64_dom_resolve(doc,base,raw,resolved,sizeof(resolved)))url=copy_text(resolved);
                else invalid_url=true;
                os64_free(raw);
            }
            if(!url)status=invalid_url?OS64_PAGE_SOURCE_EMPTY:OS64_PAGE_SOURCE_LIMIT;
            else if(os64_strlen(url)>=4u*1024u*1024u-p->image_source_bytes+old) {
                os64_free(url);url=NULL;status=OS64_PAGE_SOURCE_LIMIT;
            }
        }
    }
    const os64_html_attr_t *cors=os64_html_attr(s->node,"crossorigin");
    unsigned mode=!cors?1:os64_streq_nocase(cors->value,"use-credentials")?3:2;
    if(url && (data_address(url,os64_strlen(url)) ||
               (os64_strlen(url)>=7 && !os64_memcmp(url,"file://",7))))mode=1;
    if(url && s->url && os64_streq(url,s->url) && mode==s->mode && chosen.density==s->density) {os64_free(url);return;}
    bool authored=*image_attr(s->node,"src") || *image_attr(s->node,"srcset");
    if(!authored && status==OS64_PAGE_SOURCE_EMPTY && s->node->parent &&
       s->node->parent->ns==OS64_HTML_NS_HTML && s->node->parent->tag==OS64_HTML_TAG_PICTURE)
        for(const os64_html_node_t *n=s->node->parent->first_child;n && n!=s->node;n=n->next)
            if(n->ns==OS64_HTML_NS_HTML && n->tag==OS64_HTML_TAG_SOURCE && *image_attr(n,"srcset")) {
                authored=true;break;
            }
    bool empty=status==OS64_PAGE_SOURCE_EMPTY && !authored;
    if(!url && !s->url && s->generation && s->empty==empty &&
        s->refused==(status==OS64_PAGE_SOURCE_LIMIT))return;
    if(s->url)p->image_source_bytes-=os64_strlen(s->url)+1;
    os64_free(s->url);s->url=url;
    if(url)p->image_source_bytes+=os64_strlen(url)+1;
    s->density=chosen.density;s->mode=mode;s->generation++;
    s->display=s->native=-1;s->empty=empty;s->broken=status==OS64_PAGE_SOURCE_EMPTY && authored;
    s->refused=status==OS64_PAGE_SOURCE_LIMIT;
    if(!url) {s->current_display=s->current_native=-1;s->current_generation=s->generation;}
    if(s->refused)yonder_diag_failed_seen(p->diag,"image source","source selection exceeded its limits or has no memory");
}

static ImageSource *image_source(Page *p, const os64_html_node_t *node)
{
    for(ImageSource *s=p->image_sources;s;s=s->next)if(s->node==node) {image_source_read(p,s);return s;}
    if(p!=&g.page)return NULL;
    if(p->image_source_count>=4096) {
        yonder_diag_failed_seen(p->diag,"image source","past the selected image element limit");return NULL;
    }
    ImageSource *s=os64_calloc(1,sizeof(*s));
    if(!s){yonder_diag_failed_seen(p->diag,"image source","no memory for selected image state");return NULL;}
    os64_html_hold(page_doc(p),node);
    s->node=node;s->version=UINT64_MAX;s->display=s->native=s->current_display=s->current_native=-1;
    s->density=s->current_density=1;s->next=p->image_sources;p->image_sources=s;p->image_source_count++;
    image_source_read(p,s);return s;
}

static int32_t image_display(const Page *p, const os64_html_node_t *node, double *density)
{
    *density=1;
    // A geometry read can precede the displayed page's source update. Use a
    // completed candidate already in the catalog, without starting a request
    // or changing the shown element's current/pending pair.
    if(p->measuring) {
        os64_page_source_t chosen={0};
        if(image_choose(p,node,&chosen)==OS64_PAGE_SOURCE_OK) {
            char *raw=os64_malloc(chosen.length+1);
            if(raw) {
                os64_memcpy(raw,chosen.url,chosen.length);raw[chosen.length]=0;
                char resolved[OS64_FETCH_URL_MAX];
                const char *url=NULL;
                if(data_address(raw,chosen.length)) {os64_memcpy(raw,"data:",5);url=raw;}
                else if(os64_dom_resolve(page_doc(p),p->way.url,raw,resolved,sizeof(resolved)))url=resolved;
                const os64_html_attr_t *cors=os64_html_attr(node,"crossorigin");
                unsigned mode=!cors?0:os64_streq_nocase(cors->value,"use-credentials")?5:4;
                if(url && (data_address(url,os64_strlen(url)) ||
                           (os64_strlen(url)>=7 && !os64_memcmp(url,"file://",7))))mode=0;
                int32_t k=url && p->pic_slot?p->pic_slot[picture_slot(p,url,os64_strlen(url),mode)]:-1;
                os64_free(raw);
                if(k>=0) {
                    if(p->pics[k].state==PIC_SHOWN || p->pics[k].state==PIC_NOT_KEPT) {
                        *density=chosen.density;return k;
                    }
                }
            }
        }
    }
    for(const ImageSource *s=p->image_sources;s;s=s->next)if(s->node==node) {
        *density=s->current_density;return s->current_display;
    }
    return -1;
}

static bool image_finished(const Page *p,int32_t index)
{ return index>=0 && p->pics[index].state!=PIC_QUEUED && p->pics[index].state!=PIC_WAITING; }

static void image_sources_flush(Page *p)
{
    if(p!=&g.page || !p->modern || !p->image_sources)return;
    bool moved=false;
    for(ImageSource *s=p->image_sources;s;s=s->next) {
        image_source_read(p,s);
        if(!s->url)continue;
        if(s->wants_display && s->display<0) {
            s->display=picture_for_mode(p,s->url,s->mode==1?0:s->mode+2);
            if(s->display>=0 && !picture_sized(p,s->node))p->pics[s->display].moves=true;
        }
        if(s->wants_native && s->native<0)s->native=picture_for_mode(p,s->url,s->mode);
        bool refused=(s->wants_display && s->display<0)||(s->wants_native && s->native<0);
        if(refused) {
            s->refused=true;
            yonder_diag_failed_seen(p->diag,"image source","selected image cannot be requested: catalog limit, unsupported URL, or no memory");
        }
        // A wrapped image's completion event waits for the native product too,
        // so its load callback can synchronously draw and read back pixels.
        bool done=refused || ((!s->wants_display || image_finished(p,s->display)) &&
                             (!s->wants_native || image_finished(p,s->native)));
        if(done) {
            bool changed=s->current_display!=s->display || s->current_density!=s->density;
            bool had=s->current_display>=0 && (p->pics[s->current_display].state==PIC_SHOWN ||
                                               p->pics[s->current_display].state==PIC_NOT_KEPT);
            bool has=s->display>=0 && (p->pics[s->display].state==PIC_SHOWN ||
                                      p->pics[s->display].state==PIC_NOT_KEPT);
            // Failed initial requests do not change geometry. Initial sized
            // arrivals are painted by the arrival path without a new layout.
            moved |= changed && (had || (has && p->pics[s->display].moves));
            s->current_display=s->display;s->current_native=s->native;s->current_density=s->density;
            s->current_generation=s->generation;
        }
    }
    const os64_page_t *model=page_model(p);
    for(int32_t i=0;model && i<p->nimage_map;i++) {
        const os64_page_image_t *img=os64_page_image(model,i);if(!img)continue;
        double density;
        int32_t index=image_display(p,img->node,&density);
        if(index<0)for(const ImageSource *s=p->image_sources;s;s=s->next)
            if(s->node==img->node){index=s->display;break;}
        p->pic_of[i]=index;
    }
    if(moved){g.pictures_moved=true;layout_revision(&s_layout_resources.pictures);}
}

static void image_sources_environment(Page *p,int32_t width,int32_t height,uint32_t zoom)
{
    if(!p->modern)return;
    p->image_environment=css_view(width,height,zoom?zoom:1000);
    image_sources_flush(p);
}

static void image_sources_free(Page *p)
{
    ImageSource *s=p->image_sources;
    while(s){ImageSource *next=s->next;os64_html_release(page_doc(p),s->node);os64_free(s->url);os64_free(s);s=next;}
    p->image_sources=NULL;p->image_source_count=p->image_source_bytes=0;
}

static size_t image_sources_cost(const Page *p)
{ return p->image_source_count*sizeof(ImageSource)+p->image_source_bytes; }

static void image_sources_update(void *doc)
{
    Page *p=page_of_doc(doc);
    if(p==&g.page)pictures_feed(p);
}
