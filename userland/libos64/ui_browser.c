#include "os64/ui_browser.h"
#include "os64/fmt.h"
#include "os64/str.h"

static int32_t nonnegative(int32_t n) { return n > 0 ? n : 0; }
static int32_t smaller(int32_t a, int32_t b) { return a < b ? a : b; }
static int32_t larger(int32_t a, int32_t b) { return a > b ? a : b; }

static void request_result(os64_ui_browser_t *p, os64_ui_status_code_t rc)
{
    if (rc != OS64_UI_OK && rc != OS64_UI_BUSY)
        os64_ui_browser_message(p, "Could not open that location.");
}

static void toolbar_click(os64_ui_widget_t *w, void *user)
{
    os64_ui_browser_t *p = user;
    if (!p->browser) return;
    os64_ui_status_code_t rc = OS64_UI_INVALID;
    if (w == &p->back) rc = os64_browser_back(p->browser);
    else if (w == &p->forward) rc = os64_browser_forward(p->browser);
    else if (w == &p->up) rc = os64_browser_up(p->browser);
    else if (w == &p->refresh) rc = os64_browser_refresh(p->browser);
    request_result(p, rc);
    os64_ui_browser_sync(p);
}

static void address_submit(os64_ui_textfield_t *tf, void *user)
{
    os64_ui_browser_t *p = user;
    if (p->browser) request_result(p, os64_browser_navigate(p->browser, tf->buf));
    os64_ui_set_focus(p->ui, &p->details.w);
    os64_ui_browser_sync(p);
}

static void address_cancel(os64_ui_textfield_t *tf, void *user)
{
    os64_ui_browser_t *p = user;
    (void)tf;
    if (p->browser) os64_ui_textfield_set(p->ui, &p->address,
        os64_browser_location(p->browser));
    os64_ui_set_focus(p->ui, &p->details.w);
}

static void filter_changed(os64_ui_textfield_t *tf, void *user)
{
    os64_ui_browser_t *p = user;
    if (p->browser) request_result(p, os64_browser_filter(p->browser,
        tf->buf, os64_browser_hidden(p->browser)));
}

static void filter_cancel(os64_ui_textfield_t *tf, void *user)
{
    os64_ui_browser_t *p = user;
    (void)tf;
    os64_ui_textfield_set(p->ui, &p->filter, "");
    filter_changed(&p->filter, p);
    os64_ui_set_focus(p->ui, &p->details.w);
}

static void crumb_click(os64_ui_widget_t *w, void *user)
{
    os64_ui_browser_t *p = user;
    size_t index = (size_t)(w - p->crumbs);
    if (p->browser && index < p->crumb_count)
        request_result(p, os64_browser_navigate(p->browser,p->crumb_paths[index]));
}
static void overflow_click(os64_ui_widget_t *w,void *user)
{
    os64_ui_browser_t *p=user; (void)w;
    os64_ui_browser_focus_address(p);
}

static const char *place_label(size_t i, void *user)
{
    os64_ui_browser_t *p = user;
    const os64_directory_places_t *places = p->browser ?
        os64_browser_places(p->browser) : NULL;
    size_t index = i;
    if (places && index < places->count) return places->items[index].label;
    return "";
}

static const char *mount_label(size_t i, void *user)
{
    os64_ui_browser_t *p=user;
    const os64_directory_places_t *places=p->browser?
        os64_browser_places(p->browser):NULL;
    return place_label(i+(places?places->favorite_count:0),user);
}

static void place_changed(os64_ui_listbox_t *list, void *user)
{
    os64_ui_browser_t *p = user;
    const os64_directory_places_t *places = p->browser ?
        os64_browser_places(p->browser) : NULL;
    if (!places || list->selected < 0) return;
    size_t index = (size_t)list->selected +
        (list == &p->mounts ? places->favorite_count : 0u);
    if (index < places->count)
        request_result(p, os64_browser_navigate(p->browser,places->items[index].path));
}

void os64_ui_browser_init(os64_ui_browser_t *p, os64_ui_t *ui,
    os64_ui_widget_t *parent)
{
    *p = (os64_ui_browser_t){0};
    p->ui = ui; p->parent = parent;
    for (size_t i=0; i<OS64_UI_BROWSER_PARTS; ++i) p->visible[i] = true;
    os64_ui_panel(&p->toolbar); os64_ui_add_child(parent,&p->toolbar);
    os64_ui_widget_t *buttons[] = {&p->back,&p->forward,&p->up,&p->refresh};
    const char *labels[] = {"Back","Forward","Up","Refresh"};
    for (size_t i=0; i<4; ++i) {
        os64_ui_button(buttons[i],labels[i],toolbar_click,p);
        os64_ui_add_child(&p->toolbar,buttons[i]);
    }
    os64_ui_panel(&p->address_row); os64_ui_add_child(parent,&p->address_row);
    os64_ui_label(&p->address_label,"Location"); os64_ui_add_child(&p->address_row,&p->address_label);
    os64_ui_textfield(&p->address,p->address_text,sizeof(p->address_text),
        address_submit,address_cancel,p);
    os64_ui_add_child(&p->address_row,&p->address.w);
    os64_ui_panel(&p->crumb_row); os64_ui_add_child(parent,&p->crumb_row);
    os64_ui_button(&p->overflow,"...",overflow_click,p);
    os64_ui_add_child(&p->crumb_row,&p->overflow);
    for (size_t i=0; i<OS64_UI_BROWSER_CRUMBS; ++i) {
        os64_ui_button(&p->crumbs[i],p->crumb_names[i],crumb_click,p);
        os64_ui_add_child(&p->crumb_row,&p->crumbs[i]);
        os64_ui_set_hidden(ui,&p->crumbs[i],true);
    }
    os64_ui_panel(&p->filter_row); os64_ui_add_child(parent,&p->filter_row);
    os64_ui_label(&p->filter_label,"Filter"); os64_ui_add_child(&p->filter_row,&p->filter_label);
    os64_ui_textfield(&p->filter,p->filter_text,sizeof(p->filter_text),NULL,
        filter_cancel,p);
    p->filter.on_change = filter_changed;
    os64_ui_add_child(&p->filter_row,&p->filter.w);
    os64_ui_panel(&p->sidebar); os64_ui_add_child(parent,&p->sidebar);
    os64_ui_label(&p->favorites_label,"Favorites"); os64_ui_add_child(&p->sidebar,&p->favorites_label);
    os64_ui_listbox(&p->favorites,0,place_label,place_changed,p);
    os64_ui_add_child(&p->sidebar,&p->favorites.w);
    os64_ui_label(&p->mounts_label,"Volumes"); os64_ui_add_child(&p->sidebar,&p->mounts_label);
    os64_ui_listbox(&p->mounts,0,mount_label,place_changed,p);
    os64_ui_add_child(&p->sidebar,&p->mounts.w);
    os64_ui_dataview_init(&p->details); os64_ui_add_child(parent,&p->details.w);
    os64_ui_status_init(&p->status); os64_ui_add_child(parent,&p->status.w);
    (void)os64_ui_status_base(&p->status,"Choose a location to browse.");
}

void os64_ui_browser_bind(os64_ui_browser_t *p, os64_browser_t *browser)
{ p->browser = browser; p->seen_revision = 0; os64_ui_browser_sync(p); }

static void place(os64_ui_widget_t *w, os64_gui_rect_t r, bool staged)
{ if (staged) os64_ui_widget_stage_bounds(w,r); else w->bounds=r; }

static os64_font_status_t measured_width(os64_ui_browser_t *p,const char *text,int32_t *width)
{ return os64_ui_run_width(p->ui,NULL,OS64_FONT_ROLE_UI,text,os64_strlen(text),width); }

os64_font_status_t os64_ui_browser_layout(os64_ui_browser_t *p, bool staged)
{
    os64_gui_rect_t area = staged && p->parent->bounds_staged_valid ?
        p->parent->bounds_staged : p->parent->bounds;
    int32_t pad=p->ui->theme.pad, gap=p->ui->theme.gap;
    if (pad<0) pad=0;
    if (gap<0) gap=0;
    int32_t x=area.x+pad, y=area.y+pad;
    int32_t width=nonnegative(area.w-2*pad), bottom=area.y+area.h-pad;
    int32_t row=os64_ui_control_min_height(p->ui);
    if (row<1) row=1;
    os64_ui_widget_t *rows[]={&p->toolbar,&p->address_row,&p->crumb_row,&p->filter_row};
    os64_ui_browser_part_t kinds[]={OS64_UI_BROWSER_TOOLBAR,OS64_UI_BROWSER_ADDRESS,
        OS64_UI_BROWSER_BREADCRUMBS,OS64_UI_BROWSER_FILTER};
    int32_t ys[4], heights[4];
    for (size_t i=0;i<4;++i) {
        int32_t h=p->visible[kinds[i]] && bottom-y>row+48 ? row : 0;
        ys[i]=y; heights[i]=h;
        place(rows[i],(os64_gui_rect_t){x,y,width,h},staged);
        if (h) y+=h+gap;
    }
    os64_ui_widget_t *buttons[]={&p->back,&p->forward,&p->up,&p->refresh};
    int32_t natural[4],total=3*gap;
    for (size_t i=0;i<4;++i) {
        os64_font_status_t rc=measured_width(p,buttons[i]->text,&natural[i]);
        if(rc!=OS64_FONT_OK) return rc;
        natural[i]+=2*pad+16;
        total+=natural[i];
    }
    int32_t button_x=x,avail=width;
    for (size_t i=0;i<4;++i) {
        int32_t button_width=total<=width?natural[i]:
            smaller(natural[i],nonnegative((avail-(int32_t)(3-i)*gap)/(int32_t)(4-i)));
        place(buttons[i],(os64_gui_rect_t){button_x,ys[0],button_width,
            heights[0]},staged);
        button_x+=button_width+gap;
        avail=nonnegative(x+width-button_x);
    }
    int32_t label_width=0;
    os64_font_status_t rc=measured_width(p,"Location",&label_width);
    if(rc!=OS64_FONT_OK) return rc;
    label_width=smaller(label_width+pad,width/3);
    place(&p->address_label,(os64_gui_rect_t){x,ys[1],label_width,
        heights[1]},staged);
    place(&p->address.w,(os64_gui_rect_t){x+label_width+gap,ys[1],
        nonnegative(width-label_width-gap),heights[1]},staged);
    int32_t filter_label_width=0;
    rc=measured_width(p,"Filter",&filter_label_width);
    if(rc!=OS64_FONT_OK) return rc;
    filter_label_width=smaller(filter_label_width+pad,width/3);
    place(&p->filter_label,(os64_gui_rect_t){x,ys[3],filter_label_width,
        heights[3]},staged);
    place(&p->filter.w,(os64_gui_rect_t){x+filter_label_width+gap,ys[3],
        nonnegative(width-filter_label_width-gap),heights[3]},staged);

    int32_t status_h=p->visible[OS64_UI_BROWSER_STATUS] && bottom-y>row+24 ? row : 0;
    int32_t content_bottom=bottom-(status_h?status_h+gap:0);
    int32_t height=nonnegative(content_bottom-y);
    int32_t side_title=0;
    rc=measured_width(p,"Favorites",&side_title);
    if(rc!=OS64_FONT_OK) return rc;
    int32_t volume_title=0;
    rc=measured_width(p,"Volumes",&volume_title);
    if(rc!=OS64_FONT_OK) return rc;
    if(volume_title>side_title) side_title=volume_title;
    int32_t side_width=p->visible[OS64_UI_BROWSER_SIDEBAR] && width>=420 ?
        smaller(width/3,larger(188,side_title+4*pad+48)) : 0;
    place(&p->sidebar,(os64_gui_rect_t){x,y,side_width,height},staged);
    int32_t details_x=x+(side_width?side_width+gap:0);
    place(&p->details.w,(os64_gui_rect_t){details_x,y,
        nonnegative(width-(details_x-x)),height},staged);
    place(&p->status.w,(os64_gui_rect_t){x,content_bottom+(status_h?gap:0),
        width,status_h},staged);
    int32_t heading=os64_ui_font_row_height(p->ui,OS64_FONT_ROLE_UI)+4;
    place(&p->favorites_label,(os64_gui_rect_t){x+pad,y+pad,
        nonnegative(side_width-2*pad),heading},staged);
    place(&p->favorites.w,(os64_gui_rect_t){x+pad,y+pad+heading,
        nonnegative(side_width-2*pad),2*row+gap},staged);
    int32_t mounts_y=y+pad+heading+2*row+2*gap;
    place(&p->mounts_label,(os64_gui_rect_t){x+pad,mounts_y,
        nonnegative(side_width-2*pad),heading},staged);
    place(&p->mounts.w,(os64_gui_rect_t){x+pad,mounts_y+heading,
        nonnegative(side_width-2*pad),nonnegative(y+height-pad-(mounts_y+heading))},staged);
    int32_t crumb_widths[OS64_UI_BROWSER_CRUMBS]={0};
    for(size_t i=0;i<p->crumb_count;++i) {
        rc=measured_width(p,p->crumb_names[i],&crumb_widths[i]);
        if(rc!=OS64_FONT_OK) return rc;
        crumb_widths[i]+=2*pad+12;
    }
    int32_t root_width=p->crumb_count?smaller(crumb_widths[0],width):0;
    int32_t overflow_width=0;
    rc=measured_width(p,"...",&overflow_width);
    if(rc!=OS64_FONT_OK) return rc;
    overflow_width+=2*pad+12;
    int32_t budget=nonnegative(width-root_width-(p->crumb_count>1?gap:0));
    int32_t chosen=0;
    size_t first=p->crumb_count;
    for(size_t i=p->crumb_count;i>1;--i) {
        size_t candidate=i-1;
        int32_t needed=crumb_widths[candidate]+(chosen?gap:0)+
            (candidate>1?overflow_width+gap:0);
        if(needed>budget-chosen) break;
        chosen+=crumb_widths[candidate]+(chosen?gap:0);
        first=candidate;
    }
    if(p->crumb_count>1 && first==p->crumb_count)
        first=p->crumb_count-1;
    int32_t crumb_x=x;
    for(size_t i=0;i<OS64_UI_BROWSER_CRUMBS;++i) {
        int32_t cw=i==0?root_width:0;
        if(i>=first && i<p->crumb_count) {
            cw=smaller(crumb_widths[i],nonnegative(x+width-crumb_x));
        }
        place(&p->crumbs[i],(os64_gui_rect_t){crumb_x,ys[2],cw,
            heights[2]},staged);
        if(cw) crumb_x+=cw+gap;
        if(i==0) {
            int32_t ow=first>1?smaller(overflow_width,
                nonnegative(x+width-crumb_x)):0;
            place(&p->overflow,(os64_gui_rect_t){crumb_x,ys[2],ow,
                heights[2]},staged);
            if(ow) crumb_x+=ow+gap;
        }
    }
    if (!staged && p->details.state) (void)os64_ui_dataview_layout(&p->details);
    return OS64_FONT_OK;
}

static void breadcrumbs(os64_ui_browser_t *p, const char *path)
{
    size_t total=0;
    if (!path || !*path) path="/";
    for (size_t i=1;path[i];) { ++total; while(path[i] && path[i]!='/') ++i; if(path[i]) ++i; }
    p->crumb_count=1;
    os64_snprintf(p->crumb_names[0],sizeof(p->crumb_names[0]),"Root");
    os64_snprintf(p->crumb_paths[0],sizeof(p->crumb_paths[0]),"/");
    size_t skipped=total>OS64_UI_BROWSER_CRUMBS-1 ? total-(OS64_UI_BROWSER_CRUMBS-2) : 0;
    size_t part=0;
    for (size_t start=1; path[start];) {
        size_t end=start;
        while(path[end] && path[end]!='/') ++end;
        ++part;
        size_t target=0;
        if (skipped && part==skipped) target=1;
        else if (part>skipped || !skipped) target=part-skipped+(skipped?1:0);
        if (target && target<OS64_UI_BROWSER_CRUMBS) {
            size_t path_len=end, name_len=end-start;
            if (path_len>=OS64_DIRECTORY_PATH) path_len=OS64_DIRECTORY_PATH-1;
            for(size_t j=0;j<path_len;++j) p->crumb_paths[target][j]=path[j];
            p->crumb_paths[target][path_len]=0;
            if (skipped && part==skipped) os64_snprintf(p->crumb_names[target],
                sizeof(p->crumb_names[target]),"...");
            else {
                if(name_len>=OS64_DIRECTORY_PATH) name_len=OS64_DIRECTORY_PATH-1;
                for(size_t j=0;j<name_len;++j) {
                    unsigned char byte=(unsigned char)path[start+j];
                    p->crumb_names[target][j]=byte<32 || byte==127?'?':(char)byte;
                }
                p->crumb_names[target][name_len]=0;
            }
            if (target>=p->crumb_count) p->crumb_count=target+1;
        }
        start=path[end]?end+1:end;
    }
    for (size_t i=0;i<OS64_UI_BROWSER_CRUMBS;++i) {
        p->crumbs[i].text=p->crumb_names[i];
        os64_ui_set_hidden(p->ui,&p->crumbs[i],
            i>=p->crumb_count || !p->visible[OS64_UI_BROWSER_BREADCRUMBS]);
    }
}

void os64_ui_browser_sync(os64_ui_browser_t *p)
{
    if (!p->browser) return;
    const char *location=os64_browser_location(p->browser);
    if(!location) location="";
    uint64_t revision=os64_browser_revision(p->browser);
    const char *requested=os64_browser_requested(p->browser);
    if(!requested) requested="";
    if(!os64_streq(requested,p->seen_requested)) {
        os64_snprintf(p->seen_requested,sizeof(p->seen_requested),"%s",requested);
        if(p->help_token) {
            (void)os64_ui_status_clear_help(&p->status,p->help_token);
            p->help_token=0;
        }
    }
    if(!os64_streq(location,p->seen_location)) {
        os64_snprintf(p->seen_location,sizeof(p->seen_location),"%s",location);
        if(p->help_token) {
            (void)os64_ui_status_clear_help(&p->status,p->help_token);
            p->help_token=0;
        }
        breadcrumbs(p,location);
        os64_ui_browser_layout(p,false);
        os64_ui_mark_dirty(p->ui,&p->crumb_row);
    }
    if(p->ui->focus!=&p->address.w && !os64_streq(p->address.buf,location))
        os64_ui_textfield_set(p->ui,&p->address,location);
    if (revision!=p->seen_revision) {
        p->seen_revision=revision;
        /* Mount labels can change without changing the number of places. */
        os64_ui_mark_dirty(p->ui,&p->sidebar);
        if(p->help_token) {
            (void)os64_ui_status_clear_help(&p->status,p->help_token);
            p->help_token=0;
        }
        if (p->ui->focus!=&p->filter.w)
            os64_ui_textfield_set(p->ui,&p->filter,os64_browser_filter_text(p->browser));
    }
    const os64_directory_places_t *places=os64_browser_places(p->browser);
    size_t count=places?places->count:0;
    size_t favorites=places?places->favorite_count:0;
    if(favorites>count) favorites=count;
    if(p->favorites.count!=favorites)
        os64_ui_listbox_set(p->ui,&p->favorites,favorites,-1);
    if(p->mounts.count!=count-favorites)
        os64_ui_listbox_set(p->ui,&p->mounts,count-favorites,-1);
    char message[OS64_UI_STATUS_TEXT_MAX];
    if (os64_browser_loading(p->browser))
        os64_snprintf(message,sizeof(message),"Loading %s",os64_browser_requested(p->browser));
    else {
        int64_t io_error=0;
        os64_ui_status_code_t rc=os64_browser_status(p->browser,&io_error);
        if (rc!=OS64_UI_OK) os64_snprintf(message,sizeof(message),
            "Could not read location. Current location: %s",location);
        else {
            const os64_directory_t *dir=os64_browser_directory(p->browser);
            size_t total=dir?os64_directory_count(dir):0;
            os64_ui_selection_t *selection=os64_ui_dataview_selection(&p->details);
            os64_ui_data_projection_t *projection=selection?
                os64_ui_selection_projection(selection):NULL;
            size_t shown=projection?os64_ui_data_projection_count(projection):0;
            if(shown!=total)
                os64_snprintf(message,sizeof(message),"Showing %lu of %lu items  ·  %s",
                    (unsigned long)shown,(unsigned long)total,location);
            else os64_snprintf(message,sizeof(message),"%lu items  ·  %s",
                (unsigned long)total,location);
        }
    }
    if (!os64_streq(message,p->status_text) &&
        os64_ui_status_base(&p->status,message)==OS64_UI_OK)
        os64_snprintf(p->status_text,sizeof(p->status_text),"%s",message);
    const os64_directory_t *dir=os64_browser_directory(p->browser);
    if (dir && os64_directory_incomplete(dir) && !p->notice_token)
        (void)os64_ui_status_notice(&p->status,
            "Showing first 10,000 entries; listing incomplete",&p->notice_token);
    else if ((!dir || !os64_directory_incomplete(dir)) && p->notice_token) {
        (void)os64_ui_status_clear_notice(&p->status,p->notice_token);
        p->notice_token=0;
    }
    bool back=os64_browser_can_back(p->browser);
    bool forward=os64_browser_can_forward(p->browser);
    if(p->back.disabled==back) os64_ui_set_enabled(p->ui,&p->back,back);
    if(p->forward.disabled==forward) os64_ui_set_enabled(p->ui,&p->forward,forward);
}

void os64_ui_browser_show(os64_ui_browser_t *p,os64_ui_browser_part_t id,bool visible)
{
    if (id>=OS64_UI_BROWSER_PARTS) return;
    p->visible[id]=visible;
    os64_ui_widget_t *w[]={&p->toolbar,&p->address_row,&p->crumb_row,
        &p->filter_row,&p->sidebar,&p->status.w};
    if (!visible && p->ui->focus) {
        for (os64_ui_widget_t *node=p->ui->focus;node;node=node->parent)
            if (node==w[id]) { os64_ui_set_focus(p->ui,&p->details.w); break; }
    }
    os64_ui_set_hidden(p->ui,w[id],!visible);
    if (id==OS64_UI_BROWSER_BREADCRUMBS && visible && p->browser)
        breadcrumbs(p,os64_browser_location(p->browser));
    os64_ui_browser_layout(p,false);
    os64_ui_mark_dirty(p->ui,p->parent);
}

bool os64_ui_browser_visible(const os64_ui_browser_t *p,os64_ui_browser_part_t id)
{ return id<OS64_UI_BROWSER_PARTS && p->visible[id]; }

void os64_ui_browser_focus_address(os64_ui_browser_t *p)
{
    os64_ui_browser_show(p,OS64_UI_BROWSER_ADDRESS,true);
    os64_ui_set_focus(p->ui,&p->address.w);
}
void os64_ui_browser_focus_filter(os64_ui_browser_t *p)
{
    os64_ui_browser_show(p,OS64_UI_BROWSER_FILTER,true);
    os64_ui_set_focus(p->ui,&p->filter.w);
}
void os64_ui_browser_message(os64_ui_browser_t *p,const char *message)
{ (void)os64_ui_status_help(&p->status,message,&p->help_token); }
