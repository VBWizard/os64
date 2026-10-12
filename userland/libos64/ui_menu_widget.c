#include "ui_menu_internal.h"
#include "ui_internal.h"
#include "ui_message.h"
#include "os64/mem.h"
#include "os64/str.h"
#include "os64/fmt.h"

os64_ui_status_code_t os64_ui_menu_model_create(os64_ui_actions_t *a,
    const os64_ui_menu_entry_t *e,size_t n,os64_ui_menu_model_t **out)
{
    if(!out) return OS64_UI_INVALID;
    *out=NULL;
    if(!a||(!e&&n)) return OS64_UI_INVALID;
    if(n>OS64_UI_MENU_ENTRIES) return OS64_UI_LIMIT;
    size_t graph=n,depth=1;
    for(size_t i=0;i<n;++i) {
        if(e[i].kind<OS64_UI_MENU_ACTION||e[i].kind>OS64_UI_MENU_SUBMENU) return OS64_UI_INVALID;
        if(e[i].kind==OS64_UI_MENU_ACTION) {
            os64_ui_action_spec_t s;
            if(e[i].child||e[i].label||e[i].mnemonic||os64_ui_action_spec(a,e[i].action,&s)) return OS64_UI_INVALID;
            for(size_t j=0;j<i;++j) if(e[j].kind==OS64_UI_MENU_ACTION&&e[j].action==e[i].action) return OS64_UI_DUPLICATE;
        } else if(e[i].kind==OS64_UI_MENU_SUBMENU) {
            size_t len;
            os64_ui_status_code_t rc=ui_message_length(e[i].label,128,&len);
            if(rc) return rc;
            if(!len||e[i].action||!e[i].child||!e[i].child->count||e[i].child->actions!=a||
                (e[i].mnemonic&&(e[i].mnemonic<32||e[i].mnemonic>=127))) return OS64_UI_INVALID;
            graph+=e[i].child->graph;
            if(depth<e[i].child->depth+1) depth=e[i].child->depth+1;
        } else if(e[i].action||e[i].child||e[i].label||e[i].mnemonic) return OS64_UI_INVALID;
        if(graph>OS64_UI_MENU_GRAPH||depth>OS64_UI_MENU_DEPTH) return OS64_UI_LIMIT;
    }
    os64_ui_menu_model_t *m=os64_malloc(sizeof(*m));
    if(!m) return OS64_UI_NO_MEMORY;
    *m=(os64_ui_menu_model_t){.actions=a,.refs=1,.graph=graph,.depth=depth};
    os64_ui_status_code_t rc=os64_ui_actions_retain(a);
    if(rc) { os64_free(m); return rc; }
    for(size_t i=0;i<n;++i) {
        if(e[i].kind==OS64_UI_MENU_SEPARATOR&&(!m->count||m->entries[m->count-1].kind==OS64_UI_MENU_SEPARATOR)) continue;
        size_t k=m->count;
        m->entries[k]=e[i];
        if(e[i].child) {
            rc=os64_ui_menu_model_retain(e[i].child);
            if(rc) { os64_ui_menu_model_release(m); return rc; }
            os64_memcpy(m->labels[k],e[i].label,os64_strlen(e[i].label)+1);
            m->entries[k].label=m->labels[k];
        }
        ++m->count;
    }
    if(m->count&&m->entries[m->count-1].kind==OS64_UI_MENU_SEPARATOR) --m->count;
    *out=m; return OS64_UI_OK;
}
os64_ui_status_code_t os64_ui_menu_model_retain(os64_ui_menu_model_t *m)
{ if(!m) return OS64_UI_INVALID; if(m->refs==SIZE_MAX) return OS64_UI_LIMIT; ++m->refs; return OS64_UI_OK; }
void os64_ui_menu_model_release(os64_ui_menu_model_t *m)
{
    if(!m||--m->refs) return;
    for(size_t i=0;i<m->count;++i) if(m->entries[i].child) os64_ui_menu_model_release(m->entries[i].child);
    os64_ui_actions_release(m->actions); os64_free(m);
}
size_t os64_ui_menu_model_count(const os64_ui_menu_model_t *m) { return m?m->count:0; }
static const char *key_name(uint16_t k)
{
    switch(k) {
    case OS64_UI_KEY_ENTER:return "Enter"; case OS64_UI_KEY_ESCAPE:return "Esc";
    case OS64_UI_KEY_TAB:return "Tab"; case OS64_UI_KEY_BACKSPACE:return "Backspace";
    case OS64_UI_KEY_DELETE:return "Delete"; case OS64_UI_KEY_INSERT:return "Insert";
    case OS64_UI_KEY_UP:return "Up";case OS64_UI_KEY_DOWN:return "Down";
    case OS64_UI_KEY_LEFT:return "Left";case OS64_UI_KEY_RIGHT:return "Right";
    case OS64_UI_KEY_HOME:return "Home";case OS64_UI_KEY_END:return "End";
    case OS64_UI_KEY_PAGE_UP:return "Page Up";case OS64_UI_KEY_PAGE_DOWN:return "Page Down";
    default:return "";
    }
}
static void shortcut_text(os64_ui_shortcut_t s,char *out,size_t cap)
{
    char key[16];
    if(!s.key) { *out=0; return; }
    if(s.key>=OS64_UI_KEY_F1&&s.key<=OS64_UI_KEY_F12) os64_snprintf(key,sizeof(key),"F%u",s.key-OS64_UI_KEY_F1+1);
    else if(s.key<127) { key[0]=(char)(s.key>='a'&&s.key<='z'?s.key-32:s.key); key[1]=0; }
    else os64_snprintf(key,sizeof(key),"%s",key_name(s.key));
    os64_snprintf(out,cap,"%s%s%s%s",s.modifiers&OS64_GUI_MOD_CTRL?"Ctrl+":"",
        s.modifiers&OS64_GUI_MOD_ALT?"Alt+":"",s.modifiers&OS64_GUI_MOD_SHIFT?"Shift+":"",key);
}
/* Prepare a complete or cluster-truncated label; full descriptors stay in the
 * model. Each attempt releases its predecessor, including failed attempts. */
static os64_font_status_t fit_text(os64_ui_t *ui,const char *text,int32_t width,char *out,void **run)
{
    size_t n=os64_strlen(text);
    os64_memcpy(out,text,n+1);
    int32_t measured=0;
    os64_font_status_t rc=os64_ui_run_width(ui,run,OS64_FONT_ROLE_UI,out,n,&measured);
    if(rc||measured<=width) return rc;
    size_t cut=n;
    for(;;) {
        if(cut) { size_t start; if(!ui_text_cluster_before(text,n,cut,128,&start)) return OS64_FONT_LIMIT; cut=start; }
        if(cut>124) continue;
        os64_ui_run_release(*run); *run=NULL;
        os64_memcpy(out,text,cut); os64_memcpy(out+cut,"\xe2\x80\xa6",4);
        rc=os64_ui_run_width(ui,run,OS64_FONT_ROLE_UI,out,cut+3,&measured);
        if(rc||measured<=width) return rc;
        if(!cut) { os64_ui_run_release(*run); *run=NULL; out[0]=0; return OS64_FONT_OK; }
    }
}
bool menu_model_enabled(os64_ui_menu_model_t *m,os64_ui_action_view_t *v)
{
    if(!m||!v) return false;
    for(size_t i=0;i<m->count;++i) {
        if(m->entries[i].child) { if(menu_model_enabled(m->entries[i].child,v)) return true; }
        else if(m->entries[i].action) {
            os64_ui_action_state_t state={0};
            if(os64_ui_action_snapshot(v,m->entries[i].action,&state)==OS64_UI_OK&&state.enabled) return true;
        }
    }
    return false;
}
bool menu_selectable(os64_ui_popup_t *p,size_t i)
{
    if(i>=p->model->count) return false;
    os64_ui_menu_entry_t *e=&p->model->entries[i];
    if(e->kind==OS64_UI_MENU_SEPARATOR) return false;
    if(e->child) return menu_model_enabled(e->child,p->view);
    os64_ui_action_state_t s={0};
    return os64_ui_action_snapshot(p->view,e->action,&s)==OS64_UI_OK&&s.enabled;
}
static int32_t height(os64_ui_popup_t *p,size_t i)
{ return p->model->entries[i].kind==OS64_UI_MENU_SEPARATOR?MENU_SEP:p->row_h; }
static void viewport(os64_ui_popup_t *p)
{
    int32_t y=p->scroll?p->row_h:MENU_PAD;
    int32_t end=p->w.bounds.h-(p->scroll?p->row_h:MENU_PAD);
    p->bottom=p->top;
    while(p->bottom<p->model->count&&y+height(p,p->bottom)<=end) y+=height(p,p->bottom++);
}
void os64_ui_popup_scroll(os64_ui_popup_t *p,int32_t rows)
{
    if(!p||!p->scroll) return;
    size_t top=ui_scroll_rows(p->top,rows);
    if(top>=p->model->count) top=p->model->count-1;
    p->top=top; viewport(p);
    if(p->selected<p->top||p->selected>=p->bottom) p->selected=MENU_NONE;
}
void os64_ui_popup_select(os64_ui_popup_t *p,size_t i)
{
    if(!p) return;
    p->selected=menu_selectable(p,i)?i:MENU_NONE;
    if(p->selected==MENU_NONE) return;
    if(i<p->top) p->top=i;
    viewport(p);
    while(i>=p->bottom&&p->top<i) { ++p->top; viewport(p); }
}
size_t os64_ui_popup_selected(const os64_ui_popup_t *p) { return p?p->selected:MENU_NONE; }
os64_gui_rect_t os64_ui_popup_row(const os64_ui_popup_t *p,size_t i)
{
    if(!p||i<p->top||i>=p->bottom) return (os64_gui_rect_t){0};
    int32_t y=p->scroll?p->row_h:MENU_PAD;
    for(size_t k=p->top;k<i;++k) y+=height((os64_ui_popup_t *)p,k);
    return (os64_gui_rect_t){MENU_PAD,y,p->w.bounds.w-2*MENU_PAD,height((os64_ui_popup_t *)p,i)};
}
int os64_ui_popup_hit(const os64_ui_popup_t *p,int32_t x,int32_t y)
{
    if(!p||x<0||x>=p->w.bounds.w||y<0||y>=p->w.bounds.h) return -1;
    if(p->scroll&&y<p->row_h) return -2;
    if(p->scroll&&y>=p->w.bounds.h-p->row_h) return -3;
    for(size_t i=p->top;i<p->bottom;++i) {
        os64_gui_rect_t r=os64_ui_popup_row(p,i);
        if(y>=r.y&&y<r.y+r.h) return (int)i;
    }
    return -1;
}
static void arrow(os64_gui_surface_t *s,int32_t x,int32_t y,uint32_t fg,int direction)
{
    for(int i=0;i<4;++i) {
        if(direction==0) os64_draw_vline(s,x+i,y-3+i,7-2*i,fg);
        else os64_draw_hline(s,x-3+i,y+direction*i,7-2*i,fg);
    }
}
static void popup_paint(os64_ui_widget_t *w,os64_draw_ctx_t *ctx,const os64_ui_theme_t *t)
{
    os64_ui_popup_t *p=(void *)w;
    os64_draw_fill_rect(&ctx->surf,w->bounds,t->menu_bg);
    if(p->scroll) {
        arrow(&ctx->surf,w->bounds.x+w->bounds.w/2,w->bounds.y+p->row_h/2,t->menu_fg,-1);
        arrow(&ctx->surf,w->bounds.x+w->bounds.w/2,w->bounds.y+w->bounds.h-p->row_h/2,t->menu_fg,1);
    }
    for(size_t i=p->top;i<p->bottom;++i) {
        os64_gui_rect_t r=os64_ui_popup_row(p,i);
        r.x+=w->bounds.x; r.y+=w->bounds.y;
        os64_ui_menu_entry_t *e=&p->model->entries[i];
        if(e->kind==OS64_UI_MENU_SEPARATOR) {
            os64_draw_hline(&ctx->surf,r.x,r.y+r.h/2,r.w,t->menu_sep); continue;
        }
        bool enabled=menu_selectable(p,i),selected=i==p->selected&&enabled;
        uint32_t bg=selected?t->menu_hi_bg:t->menu_bg;
        uint32_t fg=enabled?(selected?t->menu_hi_fg:t->menu_fg):t->disabled_fg;
        os64_draw_fill_rect(&ctx->surf,r,bg);
        int32_t ty=r.y+(r.h-os64_ui_font_row_height(w->ui,OS64_FONT_ROLE_UI))/2;
        os64_ui_action_state_t state={0};
        if(e->action) (void)os64_ui_action_snapshot(p->view,e->action,&state);
        if(state.checked) {
            int32_t x=w->bounds.x+MENU_PAD+3,y=r.y+r.h/2;
            if(p->rows[i].spec.kind==OS64_UI_ACTION_RADIO) os64_draw_fill_round_rect(&ctx->surf,(os64_gui_rect_t){x,y-3,7,7},3,fg);
            else {
                for(int32_t j=0;j<3;++j) os64_draw_hline(&ctx->surf,x+j,y+j,2,fg);
                for(int32_t j=0;j<6;++j) os64_draw_hline(&ctx->surf,x+3+j,y+2-j,2,fg);
            }
        }
        if(p->icons) os64_ui_icon_draw(&ctx->surf,(os64_gui_rect_t){w->bounds.x+MENU_PAD+p->check_w+MENU_GAP,r.y+MENU_PAD,p->icon_size,p->icon_size},&p->rows[i].icon,!enabled);
        os64_ui_draw_run(w->ui,p->rows[i].run,OS64_FONT_ROLE_UI,&ctx->surf,
            (os64_gui_rect_t){w->bounds.x+p->label_x,r.y,p->label_w,r.h},w->bounds.x+p->label_x,ty,
            p->rows[i].label,os64_strlen(p->rows[i].label),fg,bg);
        os64_ui_draw_run(w->ui,p->rows[i].legend,OS64_FONT_ROLE_UI,&ctx->surf,
            (os64_gui_rect_t){w->bounds.x+p->legend_x,r.y,p->legend_w,r.h},w->bounds.x+p->legend_x,ty,
            p->rows[i].shortcut,os64_strlen(p->rows[i].shortcut),fg,bg);
        if(e->child) arrow(&ctx->surf,w->bounds.x+p->arrow_x,r.y+r.h/2,fg,0);
    }
}
static const os64_ui_class_t popup_class={.name="menu-popup",.paint=popup_paint};
os64_ui_widget_t *os64_ui_popup_widget(os64_ui_popup_t *p) { return p?&p->w:NULL; }
void os64_ui_popup_destroy(os64_ui_popup_t *p)
{
    if(!p) return;
    for(size_t i=0;i<p->model->count;++i) {
        os64_ui_run_release(p->rows[i].run); os64_ui_run_release(p->rows[i].legend);
        os64_ui_icon_release(p->provider,&p->rows[i].icon);
    }
    os64_ui_menu_model_release(p->model); os64_ui_action_view_release(p->view); os64_free(p);
}
os64_ui_status_code_t os64_ui_popup_create(os64_ui_t *ui,os64_ui_menu_model_t *m,
    os64_ui_action_view_t *v,const os64_ui_icon_provider_t *provider,int32_t mw,int32_t mh,os64_ui_popup_t **out)
{
    if(!out) return OS64_UI_INVALID;
    *out=NULL;
    if(!ui||!m||!v||!os64_ui_action_view_registry(v,m->actions)) return OS64_UI_INVALID;
    if(!m->count) return OS64_UI_EMPTY;
    os64_ui_popup_t *p=os64_malloc(sizeof(*p));
    if(!p) return OS64_UI_NO_MEMORY;
    *p=(os64_ui_popup_t){.w={.cls=&popup_class,.ui=ui},.selected=MENU_NONE,.provider=provider,.model=m,.view=v};
    os64_ui_status_code_t rc=os64_ui_menu_model_retain(m);
    if(rc) { os64_free(p); return rc; }
    rc=os64_ui_action_view_retain(v);
    if(rc) { os64_ui_menu_model_release(m); os64_free(p); return rc; }
    int32_t line=os64_ui_font_row_height(ui,OS64_FONT_ROLE_UI),label=0,legend=0;
    p->icon_size=(int32_t)os64_ui_icon_menu_size(line); p->check_w=12;
    for(size_t i=0;i<m->count;++i) {
        if(m->entries[i].action) {
            (void)os64_ui_action_spec(m->actions,m->entries[i].action,&p->rows[i].spec);
            if(p->rows[i].spec.icon) p->icons=true;
        }
    }
    p->row_h=(p->icons&&p->icon_size>line?p->icon_size:line)+2*MENU_PAD;
    int32_t total=2*MENU_PAD;
    for(size_t i=0;i<m->count;++i) {
        total+=height(p,i);
        if(m->entries[i].kind==OS64_UI_MENU_SEPARATOR) continue;
        menu_row *r=&p->rows[i];
        const char *s=m->entries[i].child?m->entries[i].label:r->spec.label;
        os64_memcpy(r->label,s,os64_strlen(s)+1); shortcut_text(r->spec.shortcut,r->shortcut,sizeof(r->shortcut));
        int32_t a=0,b=0;
        rc=ui_message_font_result(os64_ui_run_width(ui,&r->run,OS64_FONT_ROLE_UI,r->label,os64_strlen(r->label),&a));
        if(!rc) rc=ui_message_font_result(os64_ui_run_width(ui,&r->legend,OS64_FONT_ROLE_UI,r->shortcut,os64_strlen(r->shortcut),&b));
        if(rc) goto fail;
        if(a>label) label=a;
        if(b>legend) legend=b;
        if(r->spec.icon) (void)os64_ui_icon_acquire(provider,r->spec.icon,(uint32_t)p->icon_size,&r->icon);
    }
    p->scroll=total>mh;
    if(mh<(p->scroll?3*p->row_h: p->row_h+2*MENU_PAD)) { rc=OS64_UI_LIMIT; goto fail; }
    p->label_x=MENU_PAD+p->check_w+MENU_GAP+(p->icons?p->icon_size+MENU_GAP:0);
    int32_t furniture=p->label_x+3*MENU_GAP+8+MENU_PAD;
    int32_t minimum_label=0;
    rc=ui_message_font_result(os64_ui_text_measure(ui,OS64_FONT_ROLE_UI,"\xe2\x80\xa6",3,&minimum_label));
    if(rc) goto fail;
    if(minimum_label<16) minimum_label=16;
    if(mw<furniture+minimum_label) { rc=OS64_UI_LIMIT; goto fail; }
    int32_t width=furniture+label+legend;
    if(width<furniture+minimum_label) width=furniture+minimum_label;
    if(width>mw) width=mw;
    p->legend_w=legend;
    if(p->legend_w>width-furniture-minimum_label) p->legend_w=width-furniture-minimum_label;
    p->label_w=width-furniture-p->legend_w;
    p->legend_x=p->label_x+p->label_w+MENU_GAP;
    p->arrow_x=width-MENU_PAD-8;
    p->w.bounds=(os64_gui_rect_t){0,0,width,total>mh?mh:total};
    for(size_t i=0;i<m->count;++i) if(m->entries[i].kind!=OS64_UI_MENU_SEPARATOR) {
        menu_row *r=&p->rows[i];
        const char *s=m->entries[i].child?m->entries[i].label:r->spec.label;
        rc=ui_message_font_result(fit_text(ui,s,p->label_w,r->label,&r->run));
        if(rc) goto fail;
    }
    viewport(p); *out=p; return OS64_UI_OK;
fail: os64_ui_popup_destroy(p); return rc;
}

static void bar_metrics(os64_ui_widget_t *w,os64_ui_t *ui)
{ w->natural_h=os64_ui_font_row_height(ui,OS64_FONT_ROLE_UI)+2*MENU_PAD; }
static os64_font_status_t bar_prepare(os64_ui_widget_t *w,os64_ui_t *ui)
{
    os64_ui_menubar_t *b=(void *)w;
    for(size_t i=0;b->model&&i<b->model->count;++i) {
        os64_font_status_t rc=os64_ui_run_width(ui,&b->staged[i],OS64_FONT_ROLE_UI,
            b->model->entries[i].label,os64_strlen(b->model->entries[i].label),&b->staged_widths[i]);
        if(rc) return rc;
    }
    return os64_ui_run_width(ui,&w->run_staged,OS64_FONT_ROLE_UI,"...",3,&b->staged_overflow_width);
}
static void bar_discard(os64_ui_widget_t *w)
{
    os64_ui_menubar_t *b=(void *)w;
    for(size_t i=0;i<OS64_UI_MENU_ENTRIES;++i) { os64_ui_run_release(b->staged[i]); b->staged[i]=NULL; }
    os64_ui_discard_caption(w);
}
static void bar_commit(os64_ui_widget_t *w)
{
    os64_ui_menubar_t *b=(void *)w;
    for(size_t i=0;i<OS64_UI_MENU_ENTRIES;++i) {
        os64_ui_run_release(b->labels[i]); b->labels[i]=b->staged[i]; b->staged[i]=NULL;
        b->widths[i]=b->staged_widths[i];
    }
    os64_ui_commit_caption(w);
    b->overflow_width=b->staged_overflow_width+2*MENU_GAP;
}
size_t menu_bar_visible(os64_ui_menubar_t *b)
{
    if(!b||!b->model) return 0;
    int32_t used=0,overflow=b->overflow_width;
    size_t i=0;
    for(;i<b->model->count;++i) {
        int32_t width=b->widths[i]+2*MENU_GAP;
        if(used+width+(i+1<b->model->count?overflow:0)>b->w.bounds.w) break;
        used+=width;
    }
    b->visible=i; return i;
}
bool menu_bar_enabled(os64_ui_menubar_t *b,size_t i)
{
    if(!b||!b->model||i>=b->model->count) return false;
    if(!b->view) return true;
    size_t visible=menu_bar_visible(b);
    if(i<visible) return menu_model_enabled(b->model->entries[i].child,b->view);
    for(size_t k=visible;k<b->model->count;++k)
        if(menu_model_enabled(b->model->entries[k].child,b->view)) return true;
    return false;
}
static void bar_paint(os64_ui_widget_t *w,os64_draw_ctx_t *ctx,const os64_ui_theme_t *t)
{
    os64_ui_menubar_t *b=(void *)w;
    os64_draw_fill_rect(&ctx->surf,w->bounds,t->menu_bg);
    size_t visible=menu_bar_visible(b);
    int32_t x=w->bounds.x;
    for(size_t i=0;b->model&&i<=visible&&i<b->model->count;++i) {
        bool over=i==visible,sel=b->session&&os64_ui_menu_active(b->session)&&
            b->session->kind==OS64_UI_MENU_BAR&&b->selected==i;
        int32_t width=over?b->overflow_width:b->widths[i]+2*MENU_GAP;
        if(width>w->bounds.x+w->bounds.w-x) width=w->bounds.x+w->bounds.w-x;
        os64_gui_rect_t r={x,w->bounds.y,width,w->bounds.h};
        bool enabled=menu_bar_enabled(b,i); sel=sel&&enabled;
        uint32_t bg=sel?t->menu_hi_bg:t->menu_bg,fg=enabled?(sel?t->menu_hi_fg:t->menu_fg):t->disabled_fg;
        os64_draw_fill_rect(&ctx->surf,r,bg);
        const char *s=over?"...":b->model->entries[i].label;
        os64_ui_draw_run(w->ui,over?w->run:b->labels[i],OS64_FONT_ROLE_UI,&ctx->surf,r,
            x+MENU_GAP,r.y+MENU_PAD,s,os64_strlen(s),fg,bg);
        x+=width;
    }
}
static void bar_destroy_widget(os64_ui_widget_t *w) { os64_ui_menubar_destroy((void *)w); }
const os64_ui_class_t os64_ui_menubar_class={.name="menubar",.paint=bar_paint,.metrics=bar_metrics,
    .prepare=bar_prepare,.commit=bar_commit,.discard=bar_discard,.destroy=bar_destroy_widget};
void os64_ui_menubar_init(os64_ui_menubar_t *b)
{ *b=(os64_ui_menubar_t){.w={.cls=&os64_ui_menubar_class,.text="...",.auto_h=true},.selected=0,.overflow_width=36}; }
void os64_ui_menubar_destroy(os64_ui_menubar_t *b)
{
    if(!b) return;
    if(b->session&&os64_ui_menu_close(b->session,OS64_UI_MENU_EXPLICIT)!=OS64_UI_OK) return;
    bar_discard(&b->w);
    os64_ui_run_release(b->w.run); b->w.run=NULL;
    for(size_t i=0;i<OS64_UI_MENU_ENTRIES;++i) { os64_ui_run_release(b->labels[i]); b->labels[i]=NULL; }
    os64_ui_menu_model_release(b->model); b->model=NULL;
}
os64_ui_status_code_t os64_ui_menubar_bind(os64_ui_menubar_t *b,os64_ui_menu_model_t *m)
{
    if(!b||!m||!b->w.ui) return OS64_UI_INVALID;
    for(size_t i=0;i<m->count;++i) if(!m->entries[i].child) return OS64_UI_INVALID;
    if(b->session&&os64_ui_menu_active(b->session)) return OS64_UI_BUSY;
    os64_ui_status_code_t rc=os64_ui_menu_model_retain(m);
    if(rc) return rc;
    os64_ui_menu_model_t *old=b->model; b->model=m;
    rc=ui_message_font_result(bar_prepare(&b->w,b->w.ui));
    if(rc) { bar_discard(&b->w); b->model=old; os64_ui_menu_model_release(m); return rc; }
    bar_commit(&b->w); os64_ui_menu_model_release(old);
    os64_ui_mark_dirty(b->w.ui,&b->w); return OS64_UI_OK;
}
