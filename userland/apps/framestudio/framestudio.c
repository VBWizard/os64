/* Frame Studio edits an independent draft. Preparation and preview must
 * succeed before the draft changes; Apply is the explicit session boundary. */
#include "os64/os64.h"
#include "os64/ui.h"
#include "os64/font_settings.h"
#include "model.h"
#include "storage.h"
#include "os64/decoration_startup.h"
#include <stdarg.h>

static os64_draw_ctx_t ctx;
static os64_ui_t ui;
static os64_ui_widget_t root,heading,intro,tabs[4],pages[4],stage,stage_label,status_label;
static os64_ui_widget_t font_label,font_path,size_label,slots_label,color_label,saved_label,name_label;
static os64_ui_widget_t prop_labels[6],delete_tail;
static char baseline_name[FRAME_NAME_MAX+1],delete_lines[2][21];
static const char *const introduction="A little character around every window. Preview first, then make it yours.";
static os64_ui_listbox_t fonts,slots,colors,saved_list;
static os64_ui_scrollbar_t font_scroll,saved_scroll;
static os64_ui_checkbox_t includes[4],match_border;
static os64_ui_slider_t props[6];
static os64_ui_colorpicker_t picker;
static os64_ui_textfield_t hex_field,name_field;
static char saved_name[FRAME_NAME_MAX+1],pending_name[FRAME_NAME_MAX+1];
static frame_saved_t saved_entries[FRAME_SAVED_MAX];
static frame_saved_t pending_entry;
static char saved_labels[FRAME_SAVED_MAX][FRAME_NAME_MAX+16];
static size_t saved_count;
static unsigned confirmation; /* 1: close, 2: replace, 3: load, 4: delete */
static char hex[8],font_text[256],size_text[80],status[200],prop_text[6][80];
static char font_names[OS64_FONT_DISCOVERY_MAX][256],slot_names[8][96];
static os64_font_catalog_t *catalog;
static frame_draft_t draft,applied,baseline;
typedef struct {frame_draft_t draft;void *bytes;size_t length;uint64_t font_revision;} snapshot_t;
static snapshot_t history[32];
static size_t history_count,history_bytes,bundle_length;
static uint64_t font_revision=1,font_sequence=1,baseline_revision=1,applied_revision;
static void *bundle;
static os64_decor_view_t view;
static uint64_t generation;
static unsigned page,color_role;
static int selected_slot;
static const void *edit_group;
static bool close_pending,applied_once,color_second,saved_baseline,baseline_deleted;
static unsigned active_sample=0,forced_state;
static bool sample_hidden[2],sample_maximized[2],sample_pinned[2];
static uint32_t sample_hover[2];
static os64_decor_capture_t capture;
static os64_gui_rect_t specimens[2];
static const char *const action_names[]={"Spacer","Close","Minimize","Maximize","Pin"};
static const char *const shapes[]={"Square","Round","Bare"};
static const char *const finishes[]={"Solid","Gradient","Grain","Stripes","Stipple"};
static const char *const color_names[]={"Active title","Inactive title","Active title text","Inactive title text","Active border","Inactive border","Close fill","Minimize fill","Maximize fill","Pin fill",
    "Close active ink","Close inactive ink","Min active ink","Min inactive ink",
    "Max active ink","Max inactive ink","Pin active ink","Pin inactive ink"};
enum { PRESET0,PRESET1,PRESET2,UNDO,APPLY,COPY_FONT,FONT_LESS,FONT_MORE,ALIGN,
       EARLIER,LATER,REMOVE,GROUP,SHAPE,SPACER,TITLE_FINISH,BORDER_FINISH,SCALE,DIRECTION,RELIEF,
       SET_HEX,REFRESH_FONTS,FOCUS_SAMPLE,RESET_SAMPLES,SAMPLE_STATE,DISCARD_CLOSE,CANCEL_CLOSE,
       COLOR_FIRST,COLOR_SECOND,SAVE,LOAD,REFRESH_SAVED,EDIT_BUTTON_COLOR,USE_STARTUP,DEFAULT_STARTUP,DELETE_SAVED,BUTTON_COUNT };
static os64_ui_widget_t buttons[BUTTON_COUNT];
static char captions[BUTTON_COUNT][128];
static int row=30,left_width=420;
static void sync_controls(void);
static void arrange(bool staged,int r,int left);
static void log_message(const char *fmt,...) OS64_PRINTF(1,2);
static void log_message(const char *fmt,...)
{
    char line[1024];
    va_list args;va_start(args,fmt);
    os64_vsnprintf(line,sizeof(line),fmt,args);va_end(args);
    /* The system log supplies its own line ending. */
    size_t n=os64_strlen(line);
    if(n && line[n-1]=='\n')line[n-1]=0;
    os64_debug_log(line);
}
static void report(const char *text)
{
    os64_strcopy(status,sizeof(status),text);
    os64_ui_mark_dirty(&ui,&status_label);
}
static uint32_t *color_pointer(os64_decor_header_t *h,unsigned index)
{
    if(index>=10 && index<18){
        unsigned action=(index-10)/2+1;
        for(unsigned i=0;i<h->button_count;++i)if(h->buttons[i].action==action)
            return (index&1)?&h->symbols[action-1].inactive:&h->symbols[action-1].active;
        return NULL;
    }
    if(index>=6) {
        for(unsigned i=0;i<h->button_count;++i)
            if(h->buttons[i].action==index-5)return &h->buttons[i].face;
        return NULL;
    }
    switch(index){case 0:return color_second?&h->active_face2:&h->active_face;
    case 1:return color_second?&h->inactive_face2:&h->inactive_face;
    case 2:return &h->active_text;case 3:return &h->inactive_text;
    case 4:return color_second?&h->active_border2:&h->active_border;
    default:return color_second?&h->inactive_border2:&h->inactive_border;}
}
static uint32_t color_value(void)
{
    uint32_t *p=color_pointer(&draft.style,color_role);
    if(!p)return 0xff606060u;
    if(color_role>=10 && !*p)return (color_role&1)?draft.style.inactive_text:draft.style.active_text;
    if(color_role>=6 && color_role<10 && !*p)
        return 0xff000000u | (((draft.style.active_face&0xfefefeu)+0x606060u)>>1);
    return *p | 0xff000000u;
}
static void put_color(frame_draft_t *next,uint32_t color)
{
    uint32_t *p=color_pointer(&next->style,color_role);
    if(!p)return;
    *p=0xff000000u | (color&0xffffffu);
    /* Bare hides the resting housing; choosing a fill makes it visible. */
    if(color_role>=6 && color_role<10)for(unsigned i=0;i<next->style.button_count;++i)
        if(next->style.buttons[i].action==color_role-5 && next->style.buttons[i].shape==OS64_DECOR_BARE)
            next->style.buttons[i].shape=OS64_DECOR_ROUND;
}
static bool same_font(const frame_draft_t *a,const frame_draft_t *b)
{
    if(a->font.size!=b->font.size)return false;
    for(unsigned i=0;i<3;++i)if(!os64_streq(a->font.face[i],b->font.face[i]))return false;
    return true;
}
static bool prepare(const frame_draft_t *candidate,void **out,size_t *length)
{
    if(bundle && same_font(candidate,&draft))
        return os64_decor_restyle(bundle,bundle_length,&candidate->style,out,length)==OS64_FONT_OK;
    os64_font_config_t config;os64_font_config_defaults(&config);
    config.roles[OS64_FONT_ROLE_UI]=candidate->font;
    os64_font_config_error_t error;
    os64_font_set_t *set=NULL;
    if(os64_font_config_prepare(os64_ui_font_context(&ui),&config,&set,&error))return false;
    os64_font_role_view_t role;
    bool ok=os64_font_set_view(set,OS64_FONT_ROLE_UI,&role)==OS64_FONT_OK &&
        os64_decor_prepare(&role,&candidate->style,out,length)==OS64_FONT_OK;
    os64_font_set_release(set);return ok;
}
static void reset_samples(void)
{
    os64_decor_capture_cancel(&capture);
    for(unsigned i=0;i<2;++i){sample_hidden[i]=false;sample_maximized[i]=false;sample_pinned[i]=false;sample_hover[i]=0;}
    active_sample=0;
    os64_ui_mark_dirty(&ui,&stage);
}
static bool dirty(void)
{return baseline_deleted || font_revision!=baseline_revision || !frame_same(&draft,&baseline);}
static void clear_history(void)
{
    for(size_t i=0;i<history_count;++i)os64_free(history[i].bytes);
    history_count=history_bytes=0;edit_group=NULL;
}
static void describe_draft(void)
{
    report(applied_once && font_revision==applied_revision && frame_same(&draft,&applied)?
        "Draft matches your last Apply.":"Preview only. Apply when it feels right.");
}
static void undo(void)
{
    if(!history_count)return;
    snapshot_t old=history[--history_count];history_bytes-=old.length;
    os64_free(bundle);bundle=old.bytes;bundle_length=old.length;draft=old.draft;font_revision=old.font_revision;
    edit_group=NULL;(void)os64_decor_validate(bundle,bundle_length,&view);
    os64_decor_capture_cancel(&capture);sync_controls();describe_draft();
}
static bool accept(const frame_draft_t *candidate,const void *group)
{
    if(frame_same(candidate,&draft) && bundle){sync_controls();return true;}
    void *next=NULL;size_t length=0;
    if(!prepare(candidate,&next,&length)){
        report("Cannot prepare that choice. Preview kept.");sync_controls();return false;
    }
    if(!group || edit_group!=group) {
        while(history_count && (history_count==32 || history_bytes+bundle_length>32u*1024u*1024u)){
            history_bytes-=history[0].length;os64_free(history[0].bytes);
            for(size_t i=1;i<history_count;++i)history[i-1]=history[i];
            --history_count;
        }
        history[history_count++]=(snapshot_t){draft,bundle,bundle_length,font_revision};history_bytes+=bundle_length;
    }else os64_free(bundle);
    if(!same_font(candidate,&draft))font_revision=++font_sequence;
    edit_group=group;draft=*candidate;
    bundle=next;bundle_length=length;
    (void)os64_decor_validate(bundle,bundle_length,&view);
    os64_decor_capture_cancel(&capture);
    sync_controls();
    describe_draft();
    return true;
}
static const char *font_at(size_t n,void *u){(void)u;return catalog && n<catalog->count?font_names[n]:"";}
static const char *slot_at(size_t n,void *u){(void)u;return n<draft.style.button_count?slot_names[n]:"";}
static const char *color_at(size_t n,void *u){(void)u;return n<18?color_names[n]:"";}
static void refresh_fonts(void)
{
    os64_font_config_t config;os64_font_config_defaults(&config);config.roles[0]=draft.font;
    os64_font_catalog_t *next=NULL;
    if(os64_font_config_discover(os64_ui_font_context(&ui),&config,&next)) {report("Could not refresh the font list.");return;}
    os64_font_catalog_release(catalog);catalog=next;
    for(size_t i=0;i<catalog->count;++i){
        const os64_font_catalog_entry_t *entry=&catalog->entries[i];
        const char *base=entry->path;
        for(const char *p=base;*p;++p)if(*p=='/')base=p+1;
        if(os64_streq(entry->path,"builtin"))os64_strcopy(font_names[i],sizeof(font_names[i]),"Built-in bitmap (16 px)");
        else os64_snprintf(font_names[i],sizeof(font_names[i]),"%s - %s%s",entry->info.family,base,entry->status?" (unavailable)":"");
    }
    sync_controls();report("Font list refreshed. Title choice kept.");
}
static void font_changed(os64_ui_listbox_t *w,void *u)
{
    (void)u;
    if(!catalog || w->selected<0 || (size_t)w->selected>=catalog->count)return;
    const os64_font_catalog_entry_t *entry=&catalog->entries[w->selected];
    if(entry->status){report("This font is unavailable.");sync_controls();return;}
    frame_draft_t next=draft;
    os64_strcopy(next.font.face[0],sizeof(next.font.face[0]),entry->path);
    if(os64_streq(entry->path,"builtin"))next.font.size=16;
    (void)accept(&next,NULL);
}
static void list_view(os64_ui_listbox_t *list,void *u)
{
    (void)u;
    os64_ui_scrollbar_t *bar=list==&fonts?&font_scroll:&saved_scroll;
    os64_ui_scrollbar_set(&ui,bar,(int64_t)list->count,
        os64_ui_listbox_rows(list,&ui.theme),(int64_t)list->top);
}
static void font_scrolled(os64_ui_scrollbar_t *w,void *u)
{(void)u;os64_ui_listbox_scroll_to(&ui,&fonts,(size_t)w->pos);}
static void slot_changed(os64_ui_listbox_t *w,void *u)
{(void)u;selected_slot=w->selected;sync_controls();}
static void color_changed(os64_ui_listbox_t *w,void *u)
{(void)u;if(w->selected>=0)color_role=(unsigned)w->selected;sync_controls();}
static void picker_changed(os64_ui_colorpicker_t *w,void *u)
{
    (void)u;frame_draft_t next=draft;put_color(&next,w->color);
    (void)accept(&next,w);
}
static void include_changed(os64_ui_checkbox_t *w,void *u)
{
    unsigned action=(unsigned)(uintptr_t)u;
    frame_draft_t next=draft;os64_decor_header_t *h=&next.style;
    if(w->checked){
        if(h->button_count==OS64_DECOR_BUTTONS_MAX){report("Eight slots are in use. Remove a spacer first.");sync_controls();return;}
        h->buttons[h->button_count++]=(os64_decor_button_t){action,action==OS64_DECOR_PIN?0:1,OS64_DECOR_BARE,0};
        selected_slot=(int)h->button_count-1;
    }else for(unsigned i=0;i<h->button_count;++i)if(h->buttons[i].action==action){
        for(unsigned n=i+1;n<h->button_count;++n)h->buttons[n-1]=h->buttons[n];
        h->buttons[--h->button_count]=(os64_decor_button_t){0};break;
    }
    (void)accept(&next,NULL);
}
static void match_changed(os64_ui_checkbox_t *w,void *u)
{(void)u;frame_draft_t next=draft;next.style.match_border=w->checked;(void)accept(&next,NULL);}
static void property_changed(os64_ui_slider_t *w,void *u)
{
    unsigned index=(unsigned)(uintptr_t)u;frame_draft_t next=draft;
    uint32_t *fields[]={&next.style.padding_y,&next.style.border,&next.style.button_size,&next.style.button_gap,&next.style.strength,&next.style.padding_x};
    *fields[index]=(uint32_t)w->value;(void)accept(&next,w);
}
static void hex_submit(os64_ui_textfield_t *w,void *u)
{
    (void)w;(void)u;
    if(os64_strlen(hex)!=6){report("Enter six hexadecimal digits, for example 203B59.");return;}
    uint32_t value=0;
    for(unsigned i=0;i<6;++i){char c=hex[i];unsigned n=c>='0'&&c<='9'?(unsigned)(c-'0'):c>='a'&&c<='f'?(unsigned)(c-'a'+10):c>='A'&&c<='F'?(unsigned)(c-'A'+10):16;
        if(n==16){report("Use digits 0-9 and letters A-F.");return;}value=value*16+n;}
    frame_draft_t next=draft;put_color(&next,value);(void)accept(&next,NULL);
}
static const char *saved_at(size_t n,void *u)
{(void)u;return n<saved_count?saved_labels[n]:"";}
static void saved_changed(os64_ui_listbox_t *w,void *u)
{
    (void)u;
    if(w->selected>=0 && (size_t)w->selected<saved_count)
        os64_ui_textfield_set(&ui,&name_field,saved_entries[w->selected].name);
    os64_ui_scrollbar_set(&ui,&saved_scroll,saved_count,os64_ui_listbox_rows(w,&ui.theme),w->top);
    sync_controls();
}
static void saved_scrolled(os64_ui_scrollbar_t *w,void *u)
{(void)u;os64_ui_listbox_scroll_to(&ui,&saved_list,(size_t)w->pos);}
static void refresh_saved(void)
{
    int count=frame_saved_list(saved_entries,FRAME_SAVED_MAX);
    saved_count=count>=0?(size_t)count:0;
    for(size_t i=0;i<saved_count;++i)os64_snprintf(saved_labels[i],sizeof(saved_labels[i]),
        "%s%s",saved_entries[i].name,saved_entries[i].included?" (included)":"");
    int selected=-1;
    for(size_t i=0;i<saved_count;++i)if(os64_streq(saved_entries[i].name,saved_name))selected=(int)i;
    os64_ui_listbox_set(&ui,&saved_list,saved_count,selected);
    os64_ui_scrollbar_set(&ui,&saved_scroll,saved_count,os64_ui_listbox_rows(&saved_list,&ui.theme),saved_list.top);
    if(count<0)report(count==-FRAME_STORE_LIMIT?"Saved collection exceeds the browser limit.":"Cannot read saved compositions.");
}
static void ask_confirmation(unsigned kind)
{
    confirmation=kind;close_pending=true;
    if(kind==4){
        /* Split long names across full-width rows instead of the short footer. */
        os64_strcopy(delete_lines[0],sizeof(delete_lines[0]),pending_name);
        size_t n=os64_strlen(pending_name);
        os64_strcopy(delete_lines[1],sizeof(delete_lines[1]),pending_name+(n>20?20:n));
    }
    os64_ui_cancel_interaction(&ui);sync_controls();
    report(kind==4?"Delete this saved file? Cannot undo.":kind==1?"Close and discard this unsaved draft?":kind==2?
        "Replace the composition with this name?":"Discard this unsaved draft and load selection?");
}
static void save_draft(bool replace)
{
    int result=frame_save(saved_name,&draft,bundle,bundle_length,replace);
    if(result==FRAME_STORE_EXISTS){ask_confirmation(2);return;}
    if(result){report(result==FRAME_STORE_INVALID?"Name: 1-40 letters, digits, spaces or -_&().":
        "Save failed. Existing file and draft kept.");return;}
    baseline=draft;baseline_revision=font_revision;saved_baseline=true;baseline_deleted=false;
    os64_strcopy(baseline_name,sizeof(baseline_name),saved_name);
    refresh_saved();sync_controls();report("Saved. Apply separately to change the session.");
    log_message("framestudio: Save OK name=%s bytes=%lu generation=%lu\n",saved_name,(unsigned long)bundle_length,generation);
}
static void load_selected(void)
{
    frame_draft_t next;void *bytes=NULL;size_t length=0;
    if(frame_load_entry(&pending_entry,&next,&bytes,&length)){
        report("Cannot load that composition. Draft kept.");return;
    }
    clear_history();os64_free(bundle);bundle=bytes;bundle_length=length;
    draft=baseline=next;font_revision=baseline_revision=++font_sequence;saved_baseline=true;baseline_deleted=false;
    os64_strcopy(baseline_name,sizeof(baseline_name),pending_name);
    (void)os64_decor_validate(bundle,bundle_length,&view);
    os64_ui_textfield_set(&ui,&name_field,pending_name);reset_samples();refresh_fonts();sync_controls();
    report(pending_entry.included?"Included composition. Save makes a personal copy.":"Loaded into preview. Apply when ready.");
    log_message("framestudio: Load OK name=%s bytes=%lu generation=%lu\n",pending_name,(unsigned long)bundle_length,generation);
}
static void delete_selected(void)
{
    int result=frame_delete(pending_name);
    if(result){report("Delete failed. Draft and selection kept.");return;}
    /* File removal is not a draft edit: Undo must not claim to restore it. */
    if(saved_baseline && os64_streq(baseline_name,pending_name)){
        saved_baseline=false;baseline_deleted=true;
    }
    report(baseline_deleted?"Deleted. Draft kept; save to keep a copy.":"Deleted. Draft and startup choice kept.");
    refresh_saved();sync_controls();
    log_message("framestudio: Delete OK name=%s dirty=%u generation=%lu\n",pending_name,dirty(),generation);
}
static void clicked(os64_ui_widget_t *w,void *u)
{
    (void)w;unsigned id=(unsigned)(uintptr_t)u;frame_draft_t next=draft;
    os64_decor_header_t *h=&next.style;
    if(id<=PRESET2){frame_preset(&next,id);(void)accept(&next,NULL);return;}
    if(id==UNDO){undo();return;}
    if(id==SAVE){save_draft(false);return;}
    if(id==USE_STARTUP || id==DEFAULT_STARTUP){
        if(id==USE_STARTUP && (!saved_baseline || dirty()))return;
        bool defaults=id==DEFAULT_STARTUP;
        int result=os64_decor_startup_save(defaults?NULL:bundle,defaults?0:bundle_length);
        report(result?"Startup save failed. Previous choice kept.":defaults?
            "Default on next boot. Session unchanged.":"Chosen for next boot. Session unchanged.");
        if(result)os64_complain("framestudio: startup %s failed result=%d generation=%lu\n",
            defaults?"default":"decoration",result,generation);
        else log_message("framestudio: startup %s result=%d generation=%lu\n",
            defaults?"default":"decoration",result,generation);
        return;
    }
    if(id==REFRESH_SAVED){report("Collection refreshed. Draft kept.");refresh_saved();sync_controls();return;}
    if(id==LOAD || id==DELETE_SAVED){
        if(saved_list.selected<0 || (size_t)saved_list.selected>=saved_count)return;
        pending_entry=saved_entries[saved_list.selected];
        if(id==DELETE_SAVED && pending_entry.included)return;
        os64_strcopy(pending_name,sizeof(pending_name),pending_entry.name);
        if(id==DELETE_SAVED)ask_confirmation(4);else if(dirty())ask_confirmation(3);else load_selected();return;
    }
    if(id==APPLY){
        if(os64_decor_apply(bundle,bundle_length,generation)==0){
            ++generation;applied=draft;applied_revision=font_revision;applied_once=true;
            report("Applied to this session. Keep experimenting.");
            log_message("framestudio: Apply OK generation=%lu font=%u buttons=%u finish=%u\n",generation,draft.font.size,draft.style.button_count,draft.style.finish);
        }else{uint64_t actual=generation;
            if(os64_decor_generation(&actual)==0 && actual!=generation){generation=actual;report("Session changed. Review draft; Apply to replace.");}
            else report("Apply refused: window size or resource limit.");
        }sync_controls();return;
    }
    if(id==COPY_FONT){os64_font_config_t config;uint64_t serial;
        if(os64_font_settings_current(&config,&serial)){report("Cannot read the current interface font.");return;}next.font=config.roles[0];}
    else if(id==FONT_LESS){if(next.font.size>8)next.font.size=next.font.size>10?next.font.size-2:8;}
    else if(id==FONT_MORE){if(next.font.size<64)next.font.size=next.font.size<62?next.font.size+2:64;}
    else if(id==ALIGN)h->align=(h->align+1)%3;
    else if(id==REFRESH_FONTS){refresh_fonts();return;}
    else if(id==FOCUS_SAMPLE){active_sample^=1;sample_hidden[active_sample]=false;os64_ui_mark_dirty(&ui,&stage);return;}
    else if(id==RESET_SAMPLES){reset_samples();return;}
    else if(id==SAMPLE_STATE){forced_state=(forced_state+1)%4;sync_controls();return;}
    else if(id==SET_HEX){hex_submit(NULL,NULL);return;}
    else if(id==EDIT_BUTTON_COLOR){
        if(selected_slot<0 || !h->buttons[selected_slot].action)return;
        color_role=h->buttons[selected_slot].action+5;page=2;color_second=false;
        sync_controls();os64_ui_listbox_scroll_to(&ui,&colors,color_role-5);
        os64_ui_set_focus(&ui,&colors.w);
        report("Fill and symbol colors: use the list arrows.");return;
    }
    else if(id==COLOR_FIRST && color_role>=10){
        uint32_t *p=color_pointer(h,color_role);if(!p)return;
        if(*p)*p=0;else put_color(&next,color_value());
    }
    else if(id==COLOR_SECOND && color_role>=10){
        color_role=6+(color_role-10)/2;color_second=false;sync_controls();return;
    }
    else if(id==COLOR_FIRST && color_role>=6){
        uint32_t *p=color_pointer(h,color_role);if(!p)return;
        if(!*p)put_color(&next,color_value());
        else if((*p>>24)==255)*p=OS64_DECOR_FACE_TRANSPARENT | (*p&0xffffffu);
        else *p=0;
    }
    else if(id==COLOR_SECOND && color_role>=6){color_role=0;color_second=false;sync_controls();return;}
    else if(id==COLOR_FIRST || id==COLOR_SECOND){color_second=id==COLOR_SECOND;edit_group=NULL;sync_controls();return;}
    else if(id==DISCARD_CLOSE){unsigned kind=confirmation;close_pending=false;sync_controls();
        if(kind==1)ui.quit=true;else if(kind==2)save_draft(true);else if(kind==3)load_selected();else if(kind==4)delete_selected();return;}
    else if(id==CANCEL_CLOSE){close_pending=false;sync_controls();report(confirmation==4?"Deletion cancelled.":"Draft kept.");return;}
    else if(id==TITLE_FINISH)h->finish=(h->finish+1)%5;
    else if(id==BORDER_FINISH)h->border_finish=(h->border_finish+1)%5;
    else if(id==SCALE)h->scale=h->scale==8?1:h->scale*2;
    else if(id==DIRECTION)h->direction^=1;
    else if(id==RELIEF)h->relief=(h->relief+1)%4;
    else if(id==SPACER){if(h->button_count==8)return;h->buttons[h->button_count++]=(os64_decor_button_t){0,1,0,0};selected_slot=(int)h->button_count-1;}
    else if(selected_slot>=0 && (unsigned)selected_slot<h->button_count){
        unsigned at=(unsigned)selected_slot;
        if(id==REMOVE){for(unsigned i=at+1;i<h->button_count;++i)h->buttons[i-1]=h->buttons[i];h->buttons[--h->button_count]=(os64_decor_button_t){0};}
        if(id==GROUP)h->buttons[at].group^=1;
        if(id==SHAPE)h->buttons[at].shape=(h->buttons[at].shape+1)%3;
        if(id==EARLIER || id==LATER){int other=selected_slot+(id==EARLIER?-1:1);
            if(other<0 || (unsigned)other>=h->button_count)return;
            os64_decor_button_t temp=h->buttons[at];h->buttons[at]=h->buttons[other];h->buttons[other]=temp;selected_slot=other;}
    }
    (void)accept(&next,NULL);
}
static void tab_clicked(os64_ui_widget_t *w,void *u)
{(void)w;page=(unsigned)(uintptr_t)u;os64_ui_cancel_gestures(&ui);sync_controls();
    if(page==2){os64_ui_set_focus(&ui,&colors.w);report("Colors: Up/Down, Home/End, PgUp/PgDn.");}
    if(page==3)report("Save or load a draft, then choose Use at startup.");}
static void set_caption(unsigned id,const char *text)
{os64_strcopy(captions[id],sizeof(captions[id]),text);}
static void sync_controls(void)
{
    for(unsigned i=0;i<BUTTON_COUNT;++i)os64_ui_set_enabled(&ui,&buttons[i],true);
    for(unsigned i=0;i<4;++i){os64_ui_set_enabled(&ui,&pages[i],true);os64_ui_set_enabled(&ui,&tabs[i],true);}
    os64_ui_set_enabled(&ui,&stage,true);
    const os64_decor_header_t *h=&draft.style;
    static const char *const tab_names[]={"Frame","Buttons","Finish","Saved"};
    static const char *const selected_names[]={"Frame *","Buttons *","Finish *","Saved *"};
    for(unsigned i=0;i<4;++i){tabs[i].text=i==page?selected_names[i]:tab_names[i];os64_ui_set_hidden(&ui,&pages[i],i!=page);}
    os64_snprintf(size_text,sizeof(size_text),"Title size: %u px%s",draft.font.size,os64_streq(draft.font.face[0],"builtin")?" (fixed)":"");
    os64_strcopy(font_text,sizeof(font_text),draft.font.face[0]);
    os64_ui_listbox_set(&ui,&fonts,catalog?catalog->count:0,os64_font_catalog_find(catalog,draft.font.face[0]));
    os64_ui_scrollbar_set(&ui,&font_scroll,catalog?(int64_t)catalog->count:0,os64_ui_listbox_rows(&fonts,&ui.theme),(int64_t)fonts.top);
    os64_ui_set_enabled(&ui,&buttons[FONT_LESS],!os64_streq(draft.font.face[0],"builtin") && draft.font.size>8);
    os64_ui_set_enabled(&ui,&buttons[FONT_MORE],!os64_streq(draft.font.face[0],"builtin") && draft.font.size<64);
    static const char *const alignment[]={"Left","Center","Right"};
    os64_snprintf(captions[ALIGN],128,"Title alignment: %s",alignment[h->align]);
    for(unsigned i=0;i<4;++i){bool found=false;for(unsigned n=0;n<h->button_count;++n)if(h->buttons[n].action==i+1)found=true;
        os64_ui_checkbox_set(&ui,&includes[i],found);}
    if(selected_slot>=(int)h->button_count)selected_slot=(int)h->button_count-1;
    for(unsigned i=0;i<h->button_count;++i)os64_snprintf(slot_names[i],sizeof(slot_names[i]),"%u. %s / %s / %s",i+1,
        action_names[h->buttons[i].action],h->buttons[i].group?"Right":"Left",shapes[h->buttons[i].shape]);
    os64_ui_listbox_set(&ui,&slots,h->button_count,selected_slot);
    bool selected=selected_slot>=0;
    for(unsigned i=EARLIER;i<=SHAPE;++i)os64_ui_set_enabled(&ui,&buttons[i],selected);
    os64_ui_set_enabled(&ui,&buttons[EARLIER],selected_slot>0);
    os64_ui_set_enabled(&ui,&buttons[LATER],selected && (unsigned)(selected_slot+1)<h->button_count);
    os64_ui_set_enabled(&ui,&buttons[SPACER],h->button_count<8);
    set_caption(GROUP,selected && h->buttons[selected_slot].group?"Group: right":"Group: left");
    os64_snprintf(captions[SHAPE],128,"Shape: %s",selected?shapes[h->buttons[selected_slot].shape]:"choose a slot");
    uint32_t values[]={h->padding_y,h->border,h->button_size,h->button_gap,h->strength,h->padding_x};
    static const char *const names[]={"Title padding","Border width","Button size","Button gap","Strength","Side padding"};
    for(unsigned i=0;i<6;++i){os64_ui_slider_set(&ui,&props[i],(int)values[i]);os64_snprintf(prop_text[i],80,"%s: %u%s",names[i],i==4?values[i]*100/64:values[i],i==4?"%":" px");}
    os64_ui_set_enabled(&ui,&buttons[EDIT_BUTTON_COLOR],selected && h->buttons[selected_slot].action!=0);
    os64_ui_listbox_set(&ui,&colors,18,(int)color_role);
    uint32_t border_finish=h->match_border?h->finish:h->border_finish;
    bool second_available=color_role<2?h->finish!=OS64_DECOR_SOLID:
        color_role>=4 && color_role<6 && border_finish!=OS64_DECOR_SOLID;
    if(!second_available)color_second=false;
    set_caption(COLOR_FIRST,color_role==2 || color_role==3?"Text color":color_second?"Color 1":"Color 1 *");
    set_caption(COLOR_SECOND,color_second?"Color 2 *":"Color 2");
    os64_ui_set_enabled(&ui,&buttons[COLOR_SECOND],second_available);
    bool patterned=h->finish>=OS64_DECOR_GRAIN || border_finish>=OS64_DECOR_GRAIN;
    os64_ui_set_enabled(&ui,&props[4].w,patterned);
    os64_ui_set_enabled(&ui,&prop_labels[4],patterned);
    os64_ui_set_enabled(&ui,&buttons[SCALE],patterned);
    os64_ui_set_enabled(&ui,&buttons[DIRECTION],h->finish==OS64_DECOR_GRADIENT || h->finish==OS64_DECOR_STRIPES ||
        border_finish==OS64_DECOR_GRADIENT || border_finish==OS64_DECOR_STRIPES);
    uint32_t *color=color_pointer(&draft.style,color_role);
    os64_ui_set_enabled(&ui,&picker.w,color!=NULL);
    os64_ui_set_enabled(&ui,&hex_field.w,color!=NULL);
    os64_ui_set_enabled(&ui,&buttons[SET_HEX],color!=NULL);
    os64_ui_set_enabled(&ui,&buttons[COLOR_FIRST],color!=NULL);
    if(color_role>=10){
        set_caption(COLOR_FIRST,!color?"Button not included":!*color?"Ink: title text":"Ink: custom");
        set_caption(COLOR_SECOND,"Button fill");
        os64_ui_set_enabled(&ui,&buttons[COLOR_SECOND],true);
    }else if(color_role>=6){
        set_caption(COLOR_FIRST,!color?"Button not included":!*color?"Fill: automatic":(*color>>24)==255?"Fill: color":"Fill: transparent");
        set_caption(COLOR_SECOND,"Frame colors");
        os64_ui_set_enabled(&ui,&buttons[COLOR_SECOND],true);
    }
    if(picker.color!=color_value())os64_ui_colorpicker_set(&ui,&picker,color_value());
    char value[8];os64_snprintf(value,sizeof(value),"%06X",color_value()&0xffffff);
    if(!os64_streq(hex,value))os64_ui_textfield_set(&ui,&hex_field,value);
    os64_snprintf(captions[TITLE_FINISH],128,"Title: %s",finishes[h->finish]);
    os64_snprintf(captions[BORDER_FINISH],128,"Border: %s",finishes[h->border_finish]);
    os64_snprintf(captions[SCALE],128,"Scale: %ux",h->scale);
    set_caption(DIRECTION,h->direction?"Across the bar":"Down the bar");
    os64_snprintf(captions[RELIEF],128,"Relief: %u",h->relief);
    os64_ui_checkbox_set(&ui,&match_border,h->match_border!=0);
    os64_ui_set_enabled(&ui,&buttons[BORDER_FINISH],!h->match_border);
    os64_ui_set_enabled(&ui,&buttons[UNDO],history_count!=0);
    os64_ui_set_enabled(&ui,&buttons[LOAD],saved_list.selected>=0 && (size_t)saved_list.selected<saved_count);
    os64_ui_set_enabled(&ui,&buttons[USE_STARTUP],saved_baseline && !dirty());
    os64_ui_set_enabled(&ui,&buttons[DELETE_SAVED],saved_list.selected>=0 &&
        (size_t)saved_list.selected<saved_count && !saved_entries[saved_list.selected].included);
    bool deleting=close_pending && confirmation==4;
    heading.text=deleting?"DELETE SAVED COMPOSITION":"FRAME STUDIO";
    intro.text=deleting?delete_lines[0]:introduction;
    os64_ui_set_hidden(&ui,&delete_tail,!deleting);
    for(unsigned i=PRESET0;i<=PRESET2;++i)os64_ui_set_hidden(&ui,&buttons[i],deleting);
    set_caption(CANCEL_CLOSE,confirmation==4?"Cancel":"Keep editing");
    name_label.text=dirty()?"Name / unsaved changes":"Composition name";
    set_caption(DISCARD_CLOSE,confirmation==4?"Delete saved":confirmation==2?"Replace saved":confirmation==3?"Load & discard":"Discard & close");
    static const char *const states[]={"Natural","Hover","Pressed","Unavailable"};
    os64_snprintf(captions[SAMPLE_STATE],128,"State: %s",states[forced_state]);
    os64_ui_set_hidden(&ui,&buttons[DISCARD_CLOSE],!close_pending);
    os64_ui_set_hidden(&ui,&buttons[CANCEL_CLOSE],!close_pending);
    os64_ui_set_hidden(&ui,&buttons[APPLY],close_pending);
    os64_ui_set_hidden(&ui,&buttons[UNDO],close_pending);
    if(close_pending){
        for(unsigned i=0;i<BUTTON_COUNT;++i)if(i!=DISCARD_CLOSE && i!=CANCEL_CLOSE)os64_ui_set_enabled(&ui,&buttons[i],false);
        for(unsigned i=0;i<4;++i){os64_ui_set_enabled(&ui,&pages[i],false);os64_ui_set_enabled(&ui,&tabs[i],false);}
        os64_ui_set_enabled(&ui,&stage,false);
    }
    stage_label.text=view.header && os64_decor_min_width(view.header)>(uint32_t)(stage.bounds.w>70?stage.bounds.w-70:0)?
        "ACTUAL SIZE / clipped; enlarge window":"ACTUAL SIZE / active + inactive";
    os64_ui_mark_dirty(&ui,&root);
}

static bool inside(os64_gui_rect_t r,int x,int y)
{return x>=r.x && y>=r.y && (int64_t)x<r.x+r.w && (int64_t)y<r.y+r.h;}
static void sample_rectangles(void)
{
    os64_gui_rect_t r=stage.bounds;
    int width=r.w-70;
    int minimum=view.header?(int)os64_decor_min_width(view.header):120;
    if(width<minimum)width=minimum;
    int height=(r.h-70)*2/3;
    if(view.header){os64_decor_insets_t i=os64_decor_insets(view.header,true);if(height<i.top+70)height=i.top+70;}
    specimens[0]=(os64_gui_rect_t){r.x+24,r.y+100,width,height};
    specimens[1]=(os64_gui_rect_t){r.x+46,r.y+26,width,height};
    for(unsigned i=0;i<2;++i)if(sample_maximized[i])specimens[i]=(os64_gui_rect_t){r.x+10,r.y+10,r.w-20,r.h-20};
}
static void sample_paint(os64_ui_widget_t *w,os64_draw_ctx_t *dc,const os64_ui_theme_t *theme)
{
    os64_gui_rect_t area=w->bounds;
    os64_draw_fill_rect(&dc->surf,area,theme->text_bg);
    for(int y=area.y+12;y<area.y+area.h;y+=20)for(int x=area.x+12;x<area.x+area.w;x+=20)
        os64_draw_fill_rect(&dc->surf,(os64_gui_rect_t){x,y,1,1},theme->panel_border);
    if(!bundle)return;
    sample_rectangles();
    os64_decor_surface_t surface={dc->surf.pixels,dc->surf.width,dc->surf.height,dc->surf.pitch_px};
    os64_decor_rect_t clip={area.x,area.y,area.w,area.h};
    static const char *const titles[]={"Your window - Caf\xc3\xa9 / R\xc3\xa9sum\xc3\xa9","A quieter window"};
    for(unsigned pass=0;pass<2;++pass){unsigned i=pass?active_sample:active_sample^1;
        if(sample_hidden[i])continue;
        os64_gui_rect_t r=specimens[i],visible;
        if(!os64_rect_intersect(r,area,&visible))continue;
        os64_draw_fill_rect(&dc->surf,visible,theme->panel_bg);
        os64_decor_insets_t inset=os64_decor_insets(view.header,true);
        int content_y=r.y+inset.top;
        for(unsigned line=0;line<4;++line){
            os64_gui_rect_t ink={r.x+22,content_y+24+(int)line*24,r.w-(line==3?140:60),line==0?9:5};
            if(os64_rect_intersect(ink,visible,&ink))os64_draw_fill_rect(&dc->surf,ink,line==0?theme->button_border:theme->panel_border);
        }
        os64_decor_state_t state={sample_hover[i],capture.window==i+1 && capture.armed?capture.action:0,0,sample_maximized[i]};
        if(i==active_sample && forced_state){
            uint32_t action=view.header->button_count?view.header->buttons[view.header->button_count-1].action:0;
            state.hover=action;state.pressed=forced_state==2?action:0;state.disabled=forced_state==3?(1u<<action):0;
        }
        (void)os64_decor_paint(&view,&surface,(os64_decor_rect_t){r.x,r.y,r.w,r.h},clip,true,i==active_sample,
            sample_pinned[i],titles[i],os64_strlen(titles[i]),false,&state);
    }
}
static bool sample_event(os64_ui_widget_t *w,os64_ui_t *context,const os64_gui_event_t *ev)
{
    (void)w;(void)context;
    if(ev->type!=OS64_GUI_EVENT_MOUSE_MOVE && ev->type!=OS64_GUI_EVENT_MOUSE_BUTTON_DOWN && ev->type!=OS64_GUI_EVENT_MOUSE_BUTTON_UP)return false;
    sample_rectangles();unsigned target=2;uint32_t hit=0;
    if(inside(stage.bounds,ev->mouse.x,ev->mouse.y))for(unsigned pass=0;pass<2;++pass){
        unsigned i=pass?active_sample^1:active_sample;
        if(!sample_hidden[i] && inside(specimens[i],ev->mouse.x,ev->mouse.y)){
            target=i;os64_decor_layout_t layout;os64_gui_rect_t r=specimens[i];
            if(os64_decor_layout(view.header,r.w,r.h,true,&layout))hit=os64_decor_hit(view.header,&layout,ev->mouse.x-r.x,ev->mouse.y-r.y);
            break;
        }
    }
    sample_hover[0]=sample_hover[1]=0;if(target<2)sample_hover[target]=hit;
    if(capture.buttons){
        unsigned owner=capture.window;
        uint32_t action=os64_decor_capture_step(&capture,ev->type==OS64_GUI_EVENT_MOUSE_MOVE?OS64_DECOR_POINTER_MOVE:
            ev->type==OS64_GUI_EVENT_MOUSE_BUTTON_DOWN?OS64_DECOR_POINTER_DOWN:OS64_DECOR_POINTER_UP,
            ev->mouse.button,target<2?target+1:0,hit);
        if(action && owner>=1 && owner<=2){unsigned i=owner-1;
            if(action==OS64_DECOR_CLOSE || action==OS64_DECOR_MINIMIZE){sample_hidden[i]=true;active_sample=i^1;report("Sample hidden. Restore samples brings it back.");}
            if(action==OS64_DECOR_MAXIMIZE)sample_maximized[i]=!sample_maximized[i];
            if(action==OS64_DECOR_PIN)sample_pinned[i]=!sample_pinned[i];
        }
    }else if(ev->type==OS64_GUI_EVENT_MOUSE_BUTTON_DOWN && target<2){
        active_sample=target;
        if(hit && ev->mouse.button==OS64_GUI_MOUSE_LEFT && forced_state!=3)
            os64_decor_capture_begin(&capture,target+1,hit,1u);
    }
    os64_ui_mark_dirty(&ui,&stage);return true;
}
static void sample_cancel(os64_ui_widget_t *w)
{(void)w;capture=(os64_decor_capture_t){0};sample_hover[0]=sample_hover[1]=0;}
static const os64_ui_class_t sample_class={.name="frame preview",.paint=sample_paint,.event=sample_event,.cancel=sample_cancel};
static void place(os64_ui_widget_t *w,int x,int y,int width,int height,bool staged)
{
    os64_gui_rect_t r={x,y,width,height};
    if(staged)os64_ui_widget_stage_bounds(w,r);else w->bounds=r;
}
static void arrange(bool staged,int r,int left)
{
    int width=(int)ctx.surf.width,height=(int)ctx.surf.height;
    int x=20,y=3*r+36,right=left+40,space=width-right-20;
    place(&root,0,0,width,height,staged);
    place(&heading,20,12,width-40,r,staged);
    place(&intro,20,12+r,width-40,r,staged);
    place(&delete_tail,20,20+2*r,width-40,r,staged);
    for(unsigned i=0;i<3;++i)place(&buttons[i],20+(width-40)*(int)i/3,20+2*r,(width-40)/3-8,r,staged);
    for(unsigned i=0;i<4;++i){place(&tabs[i],20+left*(int)i/4,y,left/4-6,r,staged);place(&pages[i],x,y+r+10,left,height-y-r-86,staged);}
    int top=y+r+14;
    place(&font_label,x+10,top,left-20,r,staged);
    place(&fonts.w,x+10,top+r,left-38,4*r,staged);
    place(&font_scroll.w,x+left-26,top+r,16,4*r,staged);
    place(&font_path,x+10,top+5*r,left-20,r,staged);
    place(&size_label,x+10,top+6*r,left-120,r,staged);
    place(&buttons[FONT_LESS],x+left-110,top+6*r,44,r,staged);
    place(&buttons[FONT_MORE],x+left-60,top+6*r,44,r,staged);
    place(&buttons[COPY_FONT],x+10,top+7*r,left-20,r,staged);
    place(&buttons[ALIGN],x+10,top+8*r,left-20,r,staged);
    for(unsigned i=0;i<2;++i){place(&prop_labels[i],x+10,top+(9+(int)i)*r,left/2,r,staged);place(&props[i].w,x+left/2,top+(9+(int)i)*r,left/2-12,r,staged);}
    place(&prop_labels[5],x+10,top+11*r,left/2,r,staged);
    place(&props[5].w,x+left/2,top+11*r,left/2-12,r,staged);
    place(&buttons[REFRESH_FONTS],x+10,top+12*r,left-20,r,staged);
    for(unsigned i=0;i<4;++i)place(&includes[i].w,x+10+(int)(i%2)*left/2,top+(int)(i/2)*r,left/2-14,r,staged);
    place(&slots_label,x+10,top+2*r,left-20,r,staged);
    place(&slots.w,x+10,top+3*r,left-20,4*r,staged);
    for(unsigned i=0;i<3;++i)place(&buttons[EARLIER+i],x+10+(left-20)*(int)i/3,top+7*r,(left-20)/3-4,r,staged);
    place(&buttons[GROUP],x+10,top+8*r,left/2-14,r,staged);
    place(&buttons[SHAPE],x+left/2,top+8*r,left/2-10,r,staged);
    place(&buttons[SPACER],x+10,top+9*r,left-20,r,staged);
    for(unsigned i=2;i<4;++i){place(&prop_labels[i],x+10,top+(8+(int)i)*r,left/2,r,staged);place(&props[i].w,x+left/2,top+(8+(int)i)*r,left/2-12,r,staged);}
    place(&buttons[EDIT_BUTTON_COLOR],x+10,top+12*r,left-20,r,staged);
    place(&color_label,x+10,top,left-20,r,staged);
    place(&colors.w,x+10,top+r,left/2-14,6*r,staged);
    place(&picker.w,x+left/2,top+r,left/2-10,3*r,staged);
    place(&hex_field.w,x+left/2,top+4*r,left/2-100,r,staged);
    place(&buttons[SET_HEX],x+left-94,top+4*r,84,r,staged);
    place(&buttons[COLOR_FIRST],x+left/2,top+5*r,left/2-10,r,staged);
    place(&buttons[COLOR_SECOND],x+left/2,top+6*r,left/2-10,r,staged);
    place(&buttons[TITLE_FINISH],x+10,top+7*r,left-20,r,staged);
    place(&match_border.w,x+10,top+8*r,left-20,r,staged);
    place(&buttons[BORDER_FINISH],x+10,top+9*r,left-20,r,staged);
    place(&prop_labels[4],x+10,top+10*r,left/2,r,staged);
    place(&props[4].w,x+left/2,top+10*r,left/2-12,r,staged);
    place(&buttons[SCALE],x+10,top+11*r,left/2-14,r,staged);
    place(&buttons[DIRECTION],x+left/2,top+11*r,left/2-10,r,staged);
    place(&buttons[RELIEF],x+10,top+12*r,left-20,r,staged);
    place(&saved_label,x+10,top,left-20,r,staged);
    place(&saved_list.w,x+10,top+r,left-38,4*r,staged);
    place(&saved_scroll.w,x+left-26,top+r,16,4*r,staged);
    place(&name_label,x+10,top+5*r,left-20,r,staged);
    place(&name_field.w,x+10,top+6*r,left-20,r,staged);
    place(&buttons[SAVE],x+10,top+7*r,left-20,r,staged);
    place(&buttons[LOAD],x+10,top+8*r,left-20,r,staged);
    place(&buttons[REFRESH_SAVED],x+10,top+9*r,left-20,r,staged);
    place(&buttons[USE_STARTUP],x+10,top+10*r,left-20,r,staged);
    place(&buttons[DEFAULT_STARTUP],x+10,top+11*r,left-20,r,staged);
    place(&buttons[DELETE_SAVED],x+10,top+12*r,left-20,r,staged);
    place(&stage_label,right,y,space,r,staged);
    place(&stage,right,y+r+10,space,height-y-3*r-78,staged);
    place(&buttons[SAMPLE_STATE],right,height-2*r-58,space,r,staged);
    place(&buttons[FOCUS_SAMPLE],right,height-r-54,space/2-4,r,staged);
    place(&buttons[RESET_SAMPLES],right+space/2,height-r-54,space/2,r,staged);
    place(&status_label,20,height-r-10,width-520,r,staged);
    place(&buttons[UNDO],width-420,height-r-10,140,r,staged);
    place(&buttons[APPLY],width-270,height-r-10,250,r,staged);
    place(&buttons[CANCEL_CLOSE],width-480,height-r-10,200,r,staged);
    place(&buttons[DISCARD_CLOSE],width-270,height-r-10,250,r,staged);
}
typedef struct {int row,left;} layout_plan_t;
static os64_font_status_t plan_font(os64_ui_t *context,void *user,void **out)
{
    (void)user;*out=NULL;
    int r=os64_ui_font_row_height(context,OS64_FONT_ROLE_UI)+10;
    if(r<26)r=26;
    int left=r*13;if(left<390)left=390;
    if(left+500>(int)ctx.surf.width || 17*r+140>(int)ctx.surf.height)return OS64_FONT_LIMIT;
    layout_plan_t *p=os64_malloc(sizeof(*p));if(!p)return OS64_FONT_NO_MEMORY;
    *p=(layout_plan_t){r,left};arrange(true,r,left);*out=p;return OS64_FONT_OK;
}
static void discard_font(os64_ui_t *context,void *user,void *plan)
{(void)context;(void)user;os64_free(plan);}
static void commit_font(os64_ui_t *context,void *user,void *plan)
{
    (void)context;(void)user;layout_plan_t *p=plan;row=p->row;left_width=p->left;
    int minimum_w=left_width+500,minimum_h=17*row+140;
    if(minimum_w<1000)minimum_w=1000;
    if(minimum_h<650)minimum_h=650;
    if(os64_gui_window_set_min_size(ctx.win,(uint32_t)minimum_w,(uint32_t)minimum_h))ui.quit=true;
    os64_free(p);
}
static void resized(os64_ui_t *context)
{(void)context;sample_cancel(&stage);arrange(false,row,left_width);sync_controls();}
static void close_requested(os64_ui_t *context)
{
    if(!dirty()){context->quit=true;return;}
    ask_confirmation(1);
}
static void add_label(os64_ui_widget_t *w,os64_ui_widget_t *parent,const char *text)
{os64_ui_label(w,text);os64_ui_add_child(parent,w);}
static void add_button(unsigned id,os64_ui_widget_t *parent,const char *text)
{set_caption(id,text);os64_ui_button(&buttons[id],captions[id],clicked,(void *)(uintptr_t)id);os64_ui_add_child(parent,&buttons[id]);}
static void build_ui(void)
{
    os64_ui_panel(&root);os64_ui_set_root(&ui,&root);
    add_label(&heading,&root,"FRAME STUDIO");
    add_label(&intro,&root,introduction);
    add_label(&delete_tail,&root,delete_lines[1]);
    for(unsigned i=0;i<4;++i){os64_ui_panel(&pages[i]);os64_ui_add_child(&root,&pages[i]);os64_ui_button(&tabs[i],"",tab_clicked,(void *)(uintptr_t)i);os64_ui_add_child(&root,&tabs[i]);}
    add_button(PRESET0,&root,"Midnight Enamel");add_button(PRESET1,&root,"Paper & Graphite");add_button(PRESET2,&root,"Workbench");
    add_button(UNDO,&root,"Undo");add_button(APPLY,&root,"Apply to session");
    add_button(DISCARD_CLOSE,&root,"Discard & close");add_button(CANCEL_CLOSE,&root,"Keep editing");
    add_label(&font_label,&pages[0],"Title font (independent of interface)");add_label(&font_path,&pages[0],font_text);add_label(&size_label,&pages[0],size_text);
    os64_ui_listbox(&fonts,0,font_at,font_changed,NULL);fonts.on_view=list_view;os64_ui_add_child(&pages[0],&fonts.w);
    os64_ui_scrollbar(&font_scroll,font_scrolled,NULL);os64_ui_add_child(&pages[0],&font_scroll.w);
    add_button(FONT_LESS,&pages[0],"-");add_button(FONT_MORE,&pages[0],"+");add_button(COPY_FONT,&pages[0],"Copy interface font");add_button(ALIGN,&pages[0],"");add_button(REFRESH_FONTS,&pages[0],"Refresh fonts");
    for(unsigned i=0;i<4;++i){os64_ui_checkbox(&includes[i],action_names[i+1],false,include_changed,(void *)(uintptr_t)(i+1));os64_ui_add_child(&pages[1],&includes[i].w);}
    add_label(&slots_label,&pages[1],"Slots: left-to-right inside each group");
    os64_ui_listbox(&slots,0,slot_at,slot_changed,NULL);os64_ui_add_child(&pages[1],&slots.w);
    add_button(EARLIER,&pages[1],"Earlier");add_button(LATER,&pages[1],"Later");add_button(REMOVE,&pages[1],"Remove");add_button(GROUP,&pages[1],"");add_button(SHAPE,&pages[1],"");add_button(SPACER,&pages[1],"Add spacer");
    add_button(EDIT_BUTTON_COLOR,&pages[1],"Edit button colors...");
    add_label(&color_label,&pages[2],"Colors / arrows + Home/End");
    os64_ui_listbox(&colors,18,color_at,color_changed,NULL);os64_ui_add_child(&pages[2],&colors.w);
    os64_ui_colorpicker(&picker,0xff203b59,picker_changed,NULL);os64_ui_add_child(&pages[2],&picker.w);
    os64_ui_textfield(&hex_field,hex,sizeof(hex),hex_submit,NULL,NULL);os64_ui_add_child(&pages[2],&hex_field.w);
    add_button(SET_HEX,&pages[2],"Set");
    add_button(COLOR_FIRST,&pages[2],"Color 1");add_button(COLOR_SECOND,&pages[2],"Color 2");
    add_label(&saved_label,&pages[3],"Personal + included compositions");
    os64_ui_listbox(&saved_list,0,saved_at,saved_changed,NULL);saved_list.on_view=list_view;os64_ui_add_child(&pages[3],&saved_list.w);
    os64_ui_scrollbar(&saved_scroll,saved_scrolled,NULL);os64_ui_add_child(&pages[3],&saved_scroll.w);
    add_label(&name_label,&pages[3],"Composition name");
    os64_ui_textfield(&name_field,saved_name,sizeof(saved_name),NULL,NULL,NULL);os64_ui_add_child(&pages[3],&name_field.w);
    add_button(SAVE,&pages[3],"Save draft");add_button(LOAD,&pages[3],"Load selected");add_button(REFRESH_SAVED,&pages[3],"Refresh collection");
    add_button(TITLE_FINISH,&pages[2],"");add_button(BORDER_FINISH,&pages[2],"");add_button(SCALE,&pages[2],"");add_button(DIRECTION,&pages[2],"");add_button(RELIEF,&pages[2],"");
    os64_ui_checkbox(&match_border,"Use title finish on border",false,match_changed,NULL);os64_ui_add_child(&pages[2],&match_border.w);
    int min[]={0,1,16,0,0,0},max[]={32,16,48,16,64,32};
    for(unsigned i=0;i<6;++i){os64_ui_widget_t *parent=&pages[i<2 || i==5?0:i<4?1:2];
        add_label(&prop_labels[i],parent,prop_text[i]);os64_ui_slider(&props[i],min[i],max[i],1,min[i],property_changed,(void *)(uintptr_t)i);os64_ui_add_child(parent,&props[i].w);}
    add_button(DELETE_SAVED,&pages[3],"Delete selected");
    add_button(USE_STARTUP,&pages[3],"Use at startup");add_button(DEFAULT_STARTUP,&pages[3],"Restore default startup");
    add_label(&stage_label,&root,"ACTUAL SIZE / active + inactive");stage.cls=&sample_class;os64_ui_add_child(&root,&stage);
    add_button(SAMPLE_STATE,&root,"");add_button(FOCUS_SAMPLE,&root,"Focus other");add_button(RESET_SAMPLES,&root,"Restore samples");
    add_label(&status_label,&root,status);
}
int main(int argc,char **argv)
{
    (void)argc;(void)argv;
    int64_t win=os64_gui_window_create_content("Frame Studio",48,60,1240,840,OS64_GUI_CREATE_FIT_SCREEN);
    if(win<=0)return 1;
    if(os64_draw_ctx_init(&ctx,win) || ctx.surf.width<1000 || ctx.surf.height<650){os64_complain("framestudio: needs at least 1000x650 content pixels\n");os64_gui_window_destroy(win);return 1;}
    os64_ui_init(&ui,&ctx);os64_ui_theme_defaults(&ui.theme);
    os64_ui_theme_palette(&ui.theme,OS64_UI_PALETTE_MIDNIGHT);
    os64_ui_theme_current(&ui.theme,&ui.appearance_generation);
    ui.on_resize=resized;ui.on_close=close_requested;
    os64_font_config_t config;uint64_t serial;
    if(os64_font_settings_current(&config,&serial))os64_font_config_defaults(&config);
    draft.font=config.roles[0];frame_preset(&draft,0);baseline=draft;
    build_ui();arrange(false,row,left_width);
    (void)os64_gui_window_set_min_size(win,1000,650);
    (void)os64_ui_font_planner(&ui,plan_font,commit_font,discard_font,NULL);
    (void)os64_ui_font_follow(&ui);
    arrange(false,row,left_width);
    if(os64_decor_generation(&generation)){os64_free(bundle);os64_ui_font_release(&ui);os64_gui_window_destroy(win);return 1;}
    refresh_saved();
    size_t active_index=0;
    bool active_loaded=frame_load_active(saved_entries,saved_count,&active_index,&draft,&bundle,&bundle_length,&generation)==1;
    if(active_loaded){
        baseline=draft;saved_baseline=true;
        os64_strcopy(baseline_name,sizeof(baseline_name),saved_entries[active_index].name);
        os64_ui_textfield_set(&ui,&name_field,baseline_name);
        os64_ui_listbox_set(&ui,&saved_list,saved_count,(int)active_index);
        os64_ui_scrollbar_set(&ui,&saved_scroll,saved_count,os64_ui_listbox_rows(&saved_list,&ui.theme),saved_list.top);
    }
    if(!bundle && !prepare(&draft,&bundle,&bundle_length)){
        os64_complain("framestudio: could not prepare initial decoration\n");os64_ui_font_release(&ui);os64_gui_window_destroy(win);return 1;}
    (void)os64_decor_validate(bundle,bundle_length,&view);
    refresh_fonts();sync_controls();
    report(active_loaded?"Active composition loaded. Ready to edit.":ui.font_settings_result?"Interface font refused; editor kept its fallback.":"Midnight Enamel / pin left. Preview until Apply.");
    if(active_loaded)log_message("framestudio: active match name=%s generation=%lu\n",baseline_name,generation);
    log_message("framestudio: ready content=%ux%u title=%u buttons=%u generation=%lu\n",ctx.surf.width,ctx.surf.height,draft.font.size,draft.style.button_count,generation);
    os64_ui_paint(&ui);
    while(!ui.quit){os64_gui_event_t event;if(os64_gui_event_wait(win,&event)!=1)break;
        do{
            if(event.type==OS64_GUI_EVENT_MOUSE_BUTTON_DOWN || event.type==OS64_GUI_EVENT_KEY_UP)edit_group=NULL;
            if(event.type==OS64_GUI_EVENT_MOUSE_MOVE && ui.grab!=&stage && !close_pending)
                (void)sample_event(&stage,&ui,&event);
            if((event.type==OS64_GUI_EVENT_WINDOW_FOCUS && !event.focus.gained) ||
                event.type==OS64_GUI_EVENT_WINDOW_COVERED ||
                (event.type==OS64_GUI_EVENT_POINTER_STATE && !event.pointer.inside)){
                sample_cancel(&stage);os64_ui_mark_dirty(&ui,&stage);
            }
            os64_ui_dispatch(&ui,&event);
        }while(os64_gui_event_poll(win,&event)==1);
        os64_ui_paint(&ui);
    }
    log_message("framestudio: closed\n");
    clear_history();os64_font_catalog_release(catalog);os64_free(bundle);os64_ui_font_release(&ui);os64_gui_window_destroy(win);return 0;
}
