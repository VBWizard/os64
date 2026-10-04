/* Exercise the actual browser rendering/control/queue code. Only OS services
 * and the font backend are hosted; libui caret/edit code is linked for real. */
#define main js_port_fixture_main
#include "test_js_port_host.c"
#undef main
#define main yonder_fixture_main
#include "../userland/apps/yonder/yonder.c"
#undef main
#include "test_libflow_fonts.h"
#include "os64/conf.h"
#define s settings_fixture
#include "../userland/apps/yonder/settings.c"
#undef s

int64_t os64_open(const char *p, const char *m) { (void)p; (void)m; return -1; }
int64_t os64_read(int32_t h, void *b, size_t n) { (void)h; (void)b; (void)n; return -1; }
int64_t os64_close(int32_t h) { (void)h; return 0; }
int64_t os64_micros(void) { return 10000000; }
uint64_t os64_heap_verify(void) { return 0; }
void os64_debug_log(const char *text) { (void)text; }
void *os64_calloc(size_t n, size_t z) {
    if (z && n > SIZE_MAX/z) return NULL;
    void *p = os64_malloc(n*z);
    if (p) memset(p,0,n*z);
    return p;
}
const os64_font_backend_t *os64_freetype_backend_v1(void) { return flow_test_backend(); }
void os64_ui_theme_current(os64_ui_theme_t *theme, uint64_t *installed) {
    os64_ui_theme_defaults(theme); *installed=0;
}
int64_t os64_ticks(os64_ticks_t *ticks) { *ticks=(os64_ticks_t){.ticks=100,.per_second=1000}; return 0; }
uint64_t yonder_now_ms(void) { return 100; }
void yonder_ticker_set(yonder_ticker_t *ticker, uint64_t due) { (void)ticker; (void)due; }
void os64_work_cancel(os64_work_pool_t *pool, os64_work_id_t id) { (void)pool; (void)id; }
os64_work_id_t os64_work_submit(os64_work_pool_t *pool, const os64_work_t *work) {
    (void)pool; (void)work; check(false,"unexpected background submission"); return 0;
}
yonder_mail_t *yonder_mail_new(uint64_t generation) { (void)generation; return NULL; }
void yonder_mail_drop(yonder_mail_t *mail) { check(mail==NULL,"no mailbox unexpectedly retained"); }
void yonder_mail_hold(yonder_mail_t *mail) { (void)mail; }
void yonder_mail_answer(yonder_mail_t *mail, uint32_t number, bool yes) {
    (void)mail; (void)number; (void)yes;
}
#define JOB_STUB(kind) \
int64_t yonder_##kind##_run(void *job, bool (*cancelled)(void *), void *ctx, void **out) { \
    (void)job; (void)cancelled; (void)ctx; (void)out; \
    check(false,"unexpected worker execution"); return -1; \
} \
void yonder_##kind##_release(void *job, void *product) { \
    (void)job; (void)product; check(false,"unexpected worker product"); \
}
JOB_STUB(trip)
JOB_STUB(picture)
JOB_STUB(sheet)
void os64_image_free(os64_image_t *image) { os64_free(image->pixels); memset(image,0,sizeof(*image)); }
void os64_image_sequence_free(os64_image_sequence_t *sequence) { check(sequence==NULL,"no unexpected moving image"); }
const os64_image_frame_t *os64_image_sequence_frame(const os64_image_sequence_t *sequence) {
    (void)sequence; check(false,"unexpected animation read"); return NULL;
}
os64_slurp_status_t os64_slurp(const char *path, size_t cap, uint8_t **bytes, size_t *length) {
    (void)path; (void)cap; *bytes=NULL; *length=0; return OS64_SLURP_NO_FILE;
}

bool os64_ui_theme_session(os64_ui_theme_t *theme, uint64_t *installed, uint64_t hint) {
    (void)theme; (void)installed; (void)hint; return false;
}
static char saved_scripts[8], settings_report[256];
int64_t os64_conf_get(const char *file, const char *key, char *out, size_t cap) {
    check(os64_streq(file,"yonder.conf"),"settings read Yonder configuration");
    if(!os64_streq(key,"scripts") || !saved_scripts[0]) return OS64_CONF_NO_KEY;
    os64_strcopy(out,cap,saved_scripts); return 0;
}
int64_t os64_conf_set(const char *file, const char *key, const char *value) {
    check(os64_streq(file,"yonder.conf"),"settings save Yonder configuration");
    if(os64_streq(key,"scripts")) os64_strcopy(saved_scripts,sizeof(saved_scripts),value);
    return 0;
}
void os64_ui_settings_report(os64_ui_settings_t *dialog, const char *text) {
    (void)dialog; os64_strcopy(settings_report,sizeof(settings_report),text);
}
void way_cache_enable(way_cache_t *cache, bool enabled) { (void)enabled; check(cache==NULL,"no test cache"); }
void way_cache_set_cap(way_cache_t *cache, uint64_t cap) { (void)cap; check(cache==NULL,"no test cache"); }

static os64_text_context_t *probe_text;
static os64_text_font_t *probe_font;
static void *probe_alloc(void *ctx, size_t n) { (void)ctx; return os64_malloc(n); }
static void probe_free(void *ctx, void *p, size_t n) { (void)ctx; (void)n; os64_free(p); }
static os64_font_status_t probe_fonts(void *ctx, const flow_family_list_t *family,
    bool bold, bool italic, uint32_t px, os64_text_font_t *const **fonts, size_t *n,
    os64_font_face_info_t *info) {
    (void)ctx; (void)family; (void)bold; (void)italic; (void)px;
    *fonts = &probe_font; *n=1; memset(info,0,sizeof(*info));
    info->ascent=12*64; info->descent=4*64; info->line_height=20*64;
    return OS64_FONT_OK;
}
static void probe_page(const char *html, bool scripts) {
    os64_memset(&g,0,sizeof(g));
    g.scripts_on=scripts; g.way.agent=YONDER_AGENT; g.settle_due=YONDER_NEVER;
    os64_ui_init(&g.ui,NULL);
    os64_ui_panel(&g.root); os64_ui_panel(&g.view); g.view.focusable=true; os64_ui_label(&g.status,g.status_text);
    os64_ui_add_child(&g.root,&g.view); os64_ui_add_child(&g.root,&g.status);
    os64_ui_set_root(&g.ui,&g.root);
    g.view.bounds=(os64_gui_rect_t){0,0,800,600};
    g.page.way.doc=parse_file((const uint8_t *)html,strlen(html),scripts);
    g.page.scripting=scripts;
    os64_strcopy(g.page.way.url,sizeof(g.page.way.url),"https://fixture.test/page");
    g.page.way.model=os64_page_build(g.page.way.doc,g.page.way.url,NULL,NULL);
    check(g.page.way.model!=NULL,"fixture model builds");
    s_env.text=probe_text; s_env.fonts=probe_fonts; s_env.replaced_size=replaced_size;
    sheets_start(&g.page);
    check(page_lay_out(&g.page,800,600),"fixture layout builds");
    g.page.model_version=g.page.rendered_version=os64_html_version(g.page.way.doc);
    g.page.state_version=os64_page_state_version(os64_page_shared_state(g.page.way.model));
    forms_build();
    if(scripts) g.page.scripts=yonder_scripts_new(g.page.way.doc,
        os64_page_shared_state(g.page.way.model),g.page.way.url,NULL,NULL);
}
static void probe_drop(void) {
    yonder_scripts_free(g.page.scripts); g.page.scripts=NULL;
    forms_drop(); page_clear(&g.page); os64_ui_font_release(&g.ui);
}

static os64_html_node_t *probe_id(const char *id) {
    for (os64_html_node_t *n=g.page.way.doc->document;n;n=(os64_html_node_t *)next_within(n,g.page.way.doc->document)) {
        const os64_html_attr_t *a=os64_html_attr(n,"id");
        if(a && os64_streq(a->value,id)) return n;
    }
    return NULL;
}
static FormWidget *probe_field(const char *id) {
    return form_widget(os64_page_control_for(page_model(&g.page),probe_id(id)));
}
static bool probe_text_is(const char *id, const char *text) {
    os64_html_node_t *n=probe_id(id);
    return n && n->first_child && n->first_child->kind==OS64_HTML_TEXT &&
        os64_streq(n->first_child->text,text);
}
static void identity_and_edit(void) {
    probe_page("<h1 id=heading>before</h1><input id=field value=start>"
        "<script>var held=document.getElementById('field');"
        "document.getElementById('heading').textContent='first';</script>"
        "<script>if(held!==document.getElementById('field')||held.value!=='startQ')"
        "throw Error('lost edit/reference'); var added=document.createElement('input');"
        "added.id='added';document.body.insertBefore(added,held);"
        "Promise.resolve().then(()=>document.getElementById('heading').textContent='second');</script>",true);
    script_turn();
    check(probe_text_is("heading","first") && yonder_scripts_pending(g.page.scripts),
          "one script per turn, next script remains queued");
    FormWidget *field=probe_field("field");
    os64_ui_set_focus(&g.ui,field->w);
    os64_gui_event_t key={.type=OS64_GUI_EVENT_KEY_DOWN,.key={.ascii='Q'}};
    os64_ui_dispatch(&g.ui,&key);
    check(os64_streq(field->text,"startQ"),"real UI key edit remains in buffer");
    field->u.field.cursor=4; field->u.field.anchor=1; field->u.field.selected=true;
    forms_flush(); script_turn();
    check(probe_text_is("heading","second"),"Promise checkpoint completes before repaint");
    check(probe_field("field")==field && g.ui.focus==field->w &&
        field->u.field.cursor==4 && field->u.field.anchor==1 && field->u.field.selected &&
        os64_streq(field->text,"startQ"),"reordered controls keep widget, focus, caret, selection and edit");
    check(probe_field("added")!=NULL && widget_control(field)==1,"new control inserted before existing index");
    check(g.page.rendered_version==os64_html_version(g.page.way.doc),"rendered tree matches mutation");
    probe_drop();
}
static void property_only(void) {
    probe_page("<input id=field value=before><input id=tick type=checkbox>"
        "<script>document.getElementById('field').value='property';"
        "document.getElementById('tick').checked=true;</script>",true);
    uint64_t version=os64_html_version(g.page.way.doc);
    flow_tree_t *old=g.page.tree;
    script_turn();
    check(os64_html_version(g.page.way.doc)==version && g.page.tree==old,
          "state-only changes do not rebuild HTML layout");
    check(os64_streq(probe_field("field")->text,"property") &&
        probe_field("tick")->u.check.checked,"state-only changes refresh control widgets");
    probe_drop();
}
static void script_lifecycle(void) {
    const char *page="<h1 id=heading>off</h1><noscript><input id=fallback></noscript>"
        "<script>throw Error('first failed');</script>"
        "<script src=absent.js>document.getElementById('heading').textContent='external';</script>"
        "<script type=application/json>this is data</script>"
        "<template><script>document.getElementById('heading').textContent='template';</script></template>"
        "<script>document.getElementById('heading').textContent='last';</script>";
    probe_page(page,false);
    script_turn();
    check(probe_text_is("heading","off") && probe_field("fallback"),"off runs no scripts and shows noscript fallback");
    probe_drop();
    probe_page(page,true);
    check(probe_field("fallback")==NULL,"on uses scripting noscript parse policy");
    os64_html_node_t *fallback=probe_id("heading")->next;
    check(fallback && fallback->tag==OS64_HTML_TAG_NOSCRIPT &&
        flow_box_for(g.page.tree,fallback)==NULL &&
        flow_box_for(g.page.tree,fallback->first_child)==NULL,
        "scripting mode hides raw noscript fallback text");
    script_turn();
    check(probe_text_is("heading","off") && yonder_scripts_pending(g.page.scripts) &&
          strstr(g.status_text,"first failed"),"exception reported; later classic script remains usable");
    script_turn();
    check(probe_text_is("heading","last") && !yonder_scripts_pending(g.page.scripts),
          "external, data and template scripts excluded; later inline runs");
    probe_drop();
    probe_page("<h1 id=heading>before</h1><script>var held=document.body;"
        "document.getElementById('heading').textContent='first';</script>"
        "<script>document.getElementById('heading').textContent='must not run';</script>",true);
    script_turn(); stop_trip(); script_turn();
    check(probe_text_is("heading","first") && g.page.scripts==NULL,"navigation drops queued script and held wrapper");
    probe_drop();
    probe_page("<h1 id=heading>before</h1><script>function again(){return Promise.resolve().then(again)}again();</script>"
        "<script>document.getElementById('heading').textContent='must not run';</script>",true);
    script_turn();
    check(!yonder_scripts_pending(g.page.scripts) && probe_text_is("heading","before"),
          "job limit retires runtime and queued scripts");
    probe_drop();
}
static void native_refusals(void) {
    unsigned refused_model=0, refused_layout=0, survived=0;
    for(size_t cut=1;cut<=160;cut++) {
        probe_page("<style>h1{color:red}</style><h1 id=heading>old text</h1>"
            "<a id=old href=/old>old link</a><input id=field value=keep>",false);
        FormWidget *field=probe_field("field");
        os64_ui_set_focus(&g.ui,field->w);
        field->u.field.cursor=2;
        flow_tree_t *old_tree=g.page.tree;
        os64_html_node_t *heading=probe_id("heading");
        const char *old_text=heading->first_child->text;
        check(os64_html_set_text(g.page.way.doc,heading->first_child,"new text",8)==OS64_HTML_OK,"native mutation succeeds");
        fail_at=attempts+cut;
        bool rebuilt=script_rebuild();
        fail_at=0;
        if(!rebuilt) {
            check(g.page.tree==old_tree && os64_streq(old_text,"old text"),"failed rebuild retains usable pinned layout bytes");
            if(g.page.model_version!=os64_html_version(g.page.way.doc)) {
                refused_model++;
                os64_page_request_t request;
                check(os64_page_activate(page_model(&g.page),(os64_page_what_t){OS64_PAGE_ACTIVATE_LINK,0,0,0},
                    &request)==OS64_PAGE_REFUSED && request.reason==OS64_PAGE_REASON_STALE,
                    "stale model refuses activation after model allocation failure");
                os64_page_request_free(&request);
            } else refused_layout++;
            check(script_rebuild(),"retry completes without losing old page");
        } else survived++;
        check(probe_field("field")==field && field->u.field.cursor==2,"failure sweep preserves existing field identity/caret");
        probe_drop();
    }
    check(refused_model>0 && refused_layout>0 && survived>0,"allocation sweep crosses model, layout and successful rebuild paths");
    printf("Yonder refusal cuts: model=%u layout=%u completed=%u\n",refused_model,refused_layout,survived);
}

static bool settings_applied;
static void settings_capture(const char *agent, bool enabled) {
    check(os64_streq(agent,YONDER_AGENT),"settings preserves agent while applying scripts");
    settings_applied=enabled;
}
static void switch_cases(void) {
    saved_scripts[0]=0;
    check(!yonder_settings_saved_scripts(),"missing saved choice defaults off");
    strcpy(saved_scripts,"bogus");
    check(!yonder_settings_saved_scripts(),"unrecognized saved choice defaults off");
    memset(&settings_fixture,0,sizeof(settings_fixture));
    strcpy(settings_fixture.field_buf,YONDER_AGENT);
    settings_fixture.use=settings_capture;
    settings_fixture.scripts.checked=true;
    apply(&settings_fixture.d,false);
    check(settings_applied && os64_streq(saved_scripts,"bogus"),"Apply enables this window without saving default");
    apply(&settings_fixture.d,true);
    check(settings_applied && yonder_settings_saved_scripts(),"Save as default persists on for new windows");
    settings_fixture.scripts.checked=false;
    apply(&settings_fixture.d,true);
    check(!settings_applied && !yonder_settings_saved_scripts() && os64_streq(saved_scripts,"off"),
        "Save as default persists off");
    probe_page("<script>var kept=document.body;</script><script>document.body.id='later';</script>",true);
    script_turn();
    settings_use(YONDER_AGENT,false);
    check(!g.scripts_on && g.page.scripts==NULL,"applying off drops queued scripts and runtime");
    script_turn();
    check(os64_html_attr(g.page.way.doc->body,"id")==NULL,"off cannot execute remaining script");
    check(strstr(g.status_text,"SCRIPTS ON")==NULL,"off clears status indicator");
    probe_drop();
    yonder_agents_release();
}

static void edit_and_kind_changes(void) {
    probe_page("<input id=field type=number><script>document.body.setAttribute('class','first');</script>"
        "<script>document.body.setAttribute('class','second');</script>",true);
    script_turn();
    FormWidget *field=probe_field("field");
    os64_ui_textfield_set(&g.ui,&field->u.field,"-");
    field->u.field.cursor=0;
    script_turn();
    check(probe_field("field")==field && os64_streq(field->text,"-") && field->u.field.cursor==0,
        "unrelated script preserves intermediate invalid numeric edit");
    probe_drop();
    probe_page("<input id=field value=old><script>document.getElementById('field').setAttribute('type','checkbox');</script>",true);
    FormWidget *old=probe_field("field");
    script_turn();
    check(probe_field("field")!=old && probe_field("field")->kind==FW_CHECK,
        "control kind changes replace the native widget");
    probe_drop();
    probe_page("<input id=field value=old><script>document.getElementById('field').setAttribute('type','hidden');</script>",true);
    script_turn();
    check(probe_field("field")==NULL,"hidden controls have no interactive widget");
    probe_drop();
}
static void password_edits(void) {
    const char *starts[]={"abc","a",""};
    for(size_t i=0;i<3;i++) {
        char html[600];
        os64_snprintf(html,sizeof(html),"<h1 id=heading>before</h1><form action=/send>"
            "<input id=secret type=password name=p value='%s'><button id=send>Send</button></form>"
            "<script>document.getElementById('heading').textContent=document.getElementById('secret').value;</script>",starts[i]);
        probe_page(html,true);
        FormWidget *secret=probe_field("secret");
        os64_ui_set_focus(&g.ui,secret->w);
        if(i==2) {
            os64_gui_event_t a={.type=OS64_GUI_EVENT_KEY_DOWN,.key={.ascii='a'}};
            os64_gui_event_t e={.type=OS64_GUI_EVENT_KEY_DOWN,.key={.ascii=(char)0xe9}};
            check(password_key(&a) && password_key(&e),"real password keys append ASCII and Latin-1");
            forms_flush();
        }
        os64_gui_event_t back={.type=OS64_GUI_EVENT_KEY_DOWN,.key={.ascii='\b'}};
        check(password_key(&back),"real password Backspace consumed");
        forms_flush();
        const os64_page_control_t *c=os64_page_control(page_model(&g.page),widget_control(secret));
        const char *want=i==0?"ab":i==1?"":"a";
        check(c->value_len==strlen(want) && memcmp(c->value,want,c->value_len)==0,
            "initial, empty and previously flushed UTF-8 password deletions reach native state");
        os64_page_request_t request;
        char destination[128];
        os64_snprintf(destination,sizeof(destination),"https://fixture.test/send?p=%s",want);
        check(os64_page_activate(page_model(&g.page),(os64_page_what_t){OS64_PAGE_ACTIVATE_CONTROL,
            widget_control(probe_field("send")),0,0},&request)==OS64_PAGE_NAVIGATE &&
            os64_streq(request.url,destination),
            "password deletion reaches actual form request");
        os64_page_request_free(&request);
        script_turn();
        check(want[0]?probe_text_is("heading",want):probe_id("heading")->first_child==NULL,
            "next queued script observes password deletion");
        probe_drop();
    }
    probe_page("<h1 id=heading>before</h1><form action=/send><input id=secret type=password name=p>"
        "<button id=send>Send</button></form>"
        "<script>var p=document.getElementById('secret');document.getElementById('heading').textContent="
        "p.value.length===2&&p.value.charCodeAt(1)===0?'nul-kept':'lost span';</script>",true);
    FormWidget *secret=probe_field("secret");
    // The DOM setter replaces NUL with U+FFFD; the native value API also
    // accepts explicit spans, so exercise that editor projection directly.
    check(os64_page_set_text(page_model(&g.page),widget_control(secret),"a\0b",3)==0,
        "native password value accepts explicit span");
    forms_sync_from_model(true);
    check(secret->secret_len==3 && memcmp(secret->secret,"a\0b",3)==0,
        "native password value projects its full embedded-NUL span");
    os64_ui_set_focus(&g.ui,secret->w);
    os64_gui_event_t back={.type=OS64_GUI_EVENT_KEY_DOWN,.key={.ascii='\b'}};
    check(password_key(&back),"delete password byte after embedded NUL");
    script_turn();
    check(probe_text_is("heading","nul-kept"),"password edit flush preserves embedded NUL through binding");
    os64_page_request_t request;
    check(os64_page_activate(page_model(&g.page),(os64_page_what_t){OS64_PAGE_ACTIVATE_CONTROL,
        widget_control(probe_field("send")),0,0},&request)==OS64_PAGE_NAVIGATE &&
        os64_streq(request.url,"https://fixture.test/send?p=a%00"),"password span reaches encoded form request");
    os64_page_request_free(&request);
    probe_drop();

}
static void reset_editors(void) {
    probe_page("<form id=one action=/send><input id=text name=t value=seed>"
        "<input id=secret type=password name=p value=seed><input id=number type=number name=n>"
        "<button id=reset type=reset>Reset</button><button id=send>Send</button></form>"
        "<form><input id=other value=other></form><input id=owned form=one name=ext value=owned>",false);
    FormWidget *text=probe_field("text"), *secret=probe_field("secret"), *number=probe_field("number");
    FormWidget *other=probe_field("other"), *owned=probe_field("owned"), *reset=probe_field("reset");
    os64_ui_textfield_set(&g.ui,&text->u.field,"pending");
    os64_ui_textfield_set(&g.ui,&number->u.field,"-");
    os64_ui_textfield_set(&g.ui,&owned->u.field,"pending external");
    os64_ui_textfield_set(&g.ui,&other->u.field,"keep other edit");
    os64_ui_set_focus(&g.ui,secret->w);
    os64_gui_event_t key={.type=OS64_GUI_EVENT_KEY_DOWN,.key={.ascii='Q'}};
    check(password_key(&key),"unflushed password edit before reset");
    os64_ui_set_focus(&g.ui,other->w);
    other->u.field.cursor=3; other->u.field.anchor=1; other->u.field.selected=true;
    button_clicked(reset->w,reset);
    check(os64_streq(text->text,"seed") && secret->secret_len==4 &&
        memcmp(secret->secret,"seed",4)==0 && os64_streq(number->text,"") &&
        os64_streq(owned->text,"owned"),"reset discards unflushed editors, including externally owned control");
    check(os64_streq(other->text,"keep other edit") && g.ui.focus==other->w &&
        other->u.field.cursor==3 && other->u.field.selected,"reset preserves another form's edit and caret");
    forms_flush();
    os64_page_request_t request;
    check(os64_page_activate(page_model(&g.page),(os64_page_what_t){OS64_PAGE_ACTIVATE_CONTROL,
        widget_control(probe_field("send")),0,0},&request)==OS64_PAGE_NAVIGATE &&
        strstr(request.url,"t=seed") && strstr(request.url,"p=seed") &&
        strstr(request.url,"ext=owned") && !strstr(request.url,"pending"),
        "submission after reset contains defaults rather than discarded edits");
    os64_page_request_free(&request);
    os64_ui_textfield_set(&g.ui,&text->u.field,"keep refused edit");
    check(os64_html_set_attr(g.page.way.doc,g.page.way.doc->body,"class","changed",7)==OS64_HTML_OK,
        "stale reset fixture mutation");
    button_clicked(reset->w,reset);
    check(os64_streq(text->text,"keep refused edit") &&
        strstr(g.status_text,os64_page_reason_name(OS64_PAGE_REASON_STALE)),
        "refused reset retains editor and reports native refusal");
    probe_drop();
}
static void noscript_author_styles(void) {
    const char *styles[]={"display:block", "display:block!important", "display:contents!important"};
    for(size_t i=0;i<3;i++) for(int scripting=0;scripting<2;scripting++) {
        char html[512];
        os64_snprintf(html,sizeof(html),"<h1>lead</h1><style>noscript{%s}</style>"
            "<noscript id=fallback style='%s'><input id=inside value=fallback></noscript>",styles[i],styles[i]);
        probe_page(html,scripting!=0);
        os64_html_node_t *fallback=probe_id("fallback");
        check(fallback!=NULL,"noscript style fixture parsed");
        if(scripting) {
            check(flow_box_for(g.page.tree,fallback)==NULL &&
                flow_box_for(g.page.tree,fallback->first_child)==NULL,
                "author and inline display rules cannot expose scripting-mode raw fallback");
        } else {
            check(probe_field("inside")!=NULL && flow_box_for(g.page.tree,probe_id("inside"))!=NULL,
                "same author rules retain usable fallback with scripting off");
        }
        probe_drop();
    }
}
static void reclaim_browser_holders(void) {
    probe_page("<body><script>document.body.innerHTML='<h1 id=heading>replacement</h1>';</script>"
        "<script>document.getElementById('heading').textContent='removed script ran';</script>",true);
    script_turn();
    check(probe_text_is("heading","replacement") && yonder_scripts_pending(g.page.scripts),
        "replacement drops queued script after publishing a fresh layout");
    script_turn();
    check(probe_text_is("heading","replacement") && !yonder_scripts_pending(g.page.scripts),
        "queue hold permits safely skipping detached script after old layout release");
    probe_drop();

    probe_page("<div id=scroll style='overflow:auto;width:50px;height:30px'><p>content</p></div>",false);
    os64_html_node_t *scroll=probe_id("scroll");
    size_t count=g.page.way.doc->node_count;
    page_keep_box_scroll(&g.page,scroll,(flow_point_t){3,4});
    page_keep_box_scroll(&g.page,scroll,(flow_point_t){5,6});
    check(g.page.nbox_scrolls==1,"scroll identity has one retained record");
    check(os64_html_remove(g.page.way.doc,scroll)==OS64_HTML_OK && script_rebuild(),
        "scroller removal replaces native model, cascade and layout");
    check(g.page.way.doc->node_count==count && scroll->parent==NULL &&
        g.page.box_scrolls[0].node==scroll && g.page.box_scrolls[0].at.x==5,
        "scroll record retains detached identity across snapshot replacement");
    probe_drop();

    probe_page("<button id=gone type=submit>old</button><button id=keep type=submit>keep</button>",false);
    FormWidget *gone=probe_field("gone");
    os64_html_node_t *button=probe_id("gone");
    count=g.page.way.doc->node_count;
    check(os64_html_remove(g.page.way.doc,button)==OS64_HTML_OK,"detach a widget-held button");
    os64_page_t *old=page_model(&g.page), *fresh=os64_page_rebuild(old);
    check(fresh!=NULL,"widget replacement model builds");
    g.page.way.model=fresh;
    os64_page_free(old);
    flow_free(g.page.tree);g.page.tree=NULL;
    garb_cascade_free(g.page.cascade);g.page.cascade=NULL;
    g.page.sheets_changed=true;
    check(page_lay_out(&g.page,800,600),"widget replacement layout builds");
    fail_at=attempts+1;
    forms_build();
    fail_at=0;
    check(g.nfw==2 && g.fw[0]==gone,"refused widget replacement retains the previous widget");
    // Release presentation/state owners before inspecting the retained widget:
    // neither an older nor a late model pin may mask its own node hold.
    flow_free(g.page.tree);g.page.tree=NULL;
    garb_cascade_free(g.page.cascade);g.page.cascade=NULL;
    os64_page_free(g.page.way.model);g.page.way.model=NULL;
    check(g.page.way.doc->node_count==count && gone->node==button && button->parent==NULL,
        "widget alone retains detached identity after snapshot and state teardown");
    forms_drop();
    check(g.page.way.doc->node_count==count-2,
        "widget destruction reclaims button and text before document teardown");
    probe_drop();
}

static void snapshot_actions(void) {
    probe_page("<a id=old href=/old>old link</a><p>after</p>",false);
    int32_t x=0,y=0;
    bool found=false;
    for(int32_t yy=0; yy<50 && !found; yy++)
        for(int32_t xx=0; xx<180 && !found; xx++)
            if(link_at(xx,yy)==0) {x=xx;y=yy;found=true;}
    check(found,"hit-testing text inside a link resolves its ancestor");
    os64_html_node_t *old=probe_id("old");
    int64_t status=0;
    os64_html_node_t *new_link=os64_html_create_element(g.page.way.doc,OS64_HTML_NS_HTML,"a",&status);
    check(new_link && os64_html_set_attr(g.page.way.doc,new_link,"href","/wrong",6)==OS64_HTML_OK &&
        os64_html_insert(g.page.way.doc,old->parent,new_link,old)==OS64_HTML_OK &&
        os64_html_set_attr(g.page.way.doc,old,"href","/new",4)==OS64_HTML_OK,"reorder and change native links");
    os64_page_t *model=os64_page_rebuild(page_model(&g.page));
    os64_page_free(g.page.way.model); g.page.way.model=model;
    g.page.model_version=os64_html_version(g.page.way.doc);
    os64_html_pin_t pins[512]; size_t n=0;
    while(n<512 && (pins[n]=os64_html_pin(g.page.way.doc))!=0) n++;
    check(n<512 && !script_rebuild(),"layout pin exhaustion retains old snapshot with current model");
    check(flow_model(g.page.tree)!=model && link_at(x,y)==1,"old layout link index resolves through retained model node");
    os64_page_request_t request;
    check(os64_page_activate(model,(os64_page_what_t){OS64_PAGE_ACTIVATE_LINK,link_at(x,y),0,0},&request)==
        OS64_PAGE_NAVIGATE && os64_streq(request.url,"https://fixture.test/new"),"click sends current href after failed layout");
    os64_page_request_free(&request);
    for(size_t i=0;i<n;i++) os64_html_unpin(g.page.way.doc,pins[i]);
    check(os64_html_remove(g.page.way.doc,old)==OS64_HTML_OK,"detach old link");
    model=os64_page_rebuild(page_model(&g.page));
    os64_page_free(g.page.way.model); g.page.way.model=model;
    g.page.model_version=os64_html_version(g.page.way.doc);
    check(link_at(x,y)==-1,"detached old box does not activate another current link");
    probe_drop();
}
static void stylesheet_reuse(void) {
    unsigned refused=0;
    for (size_t cut=0; cut<=64; cut++) {
        probe_page("<link rel=stylesheet href='/site.css'><link rel=stylesheet href='/site.css'>"
            "<h1 id=heading>old</h1>",false);
        const char css[]="@import '/child.css'; h1{color:#123456}";
        for (int32_t i=0; i<g.page.nsheets; i++) {
            Sheet *sheet=&g.page.sheets[i];
            bool child=strstr(sheet->url,"child.css")!=NULL;
            const char *text=child?"h1{font-weight:bold}":css;
            check(garb_parse_sheet_text(text,strlen(text),&sheet->parsed)==GARB_OK,"linked sheet fixture parses");
            sheet_ready(&g.page,i);
        }
        check(g.page.nsheets==4 && page_lay_out(&g.page,800,600),"duplicate links and imports lay out");
        const void *arenas[4];
        for(int i=0; i<4; i++) arenas[i]=g.page.sheets[i].parsed.arena;
        check(os64_html_set_text(g.page.way.doc,probe_id("heading")->first_child,"new",3)==OS64_HTML_OK,
            "linked sheet fixture text changes");
        if(cut) fail_at=attempts+cut;
        bool complete=script_rebuild();
        fail_at=0;
        if(!complete) {
            refused++;
            check(g.page.sheets[0].parsed.arena==arenas[0] && g.page.sheets[0].ready,
                "failed recascade keeps original linked parse");
            check(script_rebuild(),"linked parse survives failure and retry");
        }
        bool transferred=g.page.nsheets==4;
        for(int i=0; i<g.page.nsheets; i++) {
            bool found=false;
            for(int j=0; j<4; j++) found |= g.page.sheets[i].parsed.arena==arenas[j];
            transferred &= found && g.page.sheets[i].ready && g.page.sheets[i].borrowed_from==NULL;
            for(int j=0; j<i; j++) transferred &= g.page.sheets[i].parsed.arena!=g.page.sheets[j].parsed.arena;
        }
        // An optional sheet metadata allocation can fail while geometry still
        // succeeds; that sheet is omitted and its unused old parse is released.
        if(complete && g.page.nsheets!=4) {
            check(g.page.nsheets<4,"sheet metadata refusal remains bounded");
        } else if(complete && !transferred) {
            bool omitted=false;
            for(int i=0;i<g.page.nsheets;i++) omitted |= !g.page.sheets[i].ready;
            check(omitted,"refused import metadata may omit a sheet");
        } else {
            check(transferred,"ready duplicate parses and imports transfer without aliasing ownership");
        }
        probe_drop();
    }
    check(refused>0,"linked stylesheet sweep reaches rollback");
}

static void bounded_resources(void) {
    probe_page("<img id=pic src=file:///old.gif><script>document.getElementById('pic').setAttribute('src','file:///new.gif');</script>",true);
    pictures_start(&g.page);
    check(g.page.npics==1,"initial image address catalogued");
    script_turn();
    check(g.page.npics==2 && os64_streq(g.page.pics[0].url,"file:///old.gif") &&
        os64_streq(g.page.pics[1].url,"file:///new.gif"),"catalog addresses outlive rebuilt model");
    g.page.picture_url_bytes=4*1024*1024;
    check(picture_for(&g.page,"file:///refused.gif")==-1,"catalog byte cap refuses new addresses");
    check(picture_for(&g.page,"file:///old.gif")==0,"cap still permits existing address lookup");
    probe_drop();
    probe_page("<script>var held=document.body;</script><script>document.body.id='later';</script>",true);
    yonder_scripts_free(g.page.scripts); g.page.scripts=NULL;
    fail_at=attempts+1;
    check(yonder_scripts_new(g.page.way.doc,os64_page_shared_state(page_model(&g.page)),g.page.way.url,
        NULL,NULL)==NULL,"script queue allocation refusal publishes no runtime");
    fail_at=0;
    probe_drop();
    size_t count=4097, one=sizeof("<script></script>")-1;
    char *many=malloc(count*one+1);
    check(many!=NULL,"script count fixture allocation");
    for(size_t i=0;i<count;i++) memcpy(many+i*one,"<script></script>",one);
    many[count*one]='\0';
    probe_page(many,false); free(many);
    check(yonder_scripts_new(g.page.way.doc,os64_page_shared_state(page_model(&g.page)),
        g.page.way.url,NULL,NULL)==NULL,"over-cap script schedule is refused atomically");
    probe_drop();
    probe_page("<script id=source></script><script>document.body.id='must not run';</script>",true);
    size_t bytes=4*1024*1024+1;
    char *huge=malloc(bytes);
    check(huge!=NULL,"script source fixture allocation");
    memset(huge,' ',bytes);
    int64_t status;
    os64_html_node_t *source=os64_html_create_text(g.page.way.doc,huge,bytes,&status);
    check(source && status==OS64_HTML_OK &&
        os64_html_insert(g.page.way.doc,probe_id("source"),source,NULL)==OS64_HTML_OK,
        "oversized inline source staged natively");
    free(huge);
    os64_js_outcome_t out;
    check(yonder_scripts_step(g.page.scripts,&out) && out.status==OS64_JS_LIMIT &&
        out.limit==OS64_JS_LIMIT_SOURCE && strstr(out.source_name,"#inline-1") &&
        !yonder_scripts_pending(g.page.scripts),"source cap reports location and retires later scripts");
    probe_drop();
}

int main(void)
{
    os64_text_options_t options={.memory={.alloc=probe_alloc,.free=probe_free},
        .backend=flow_test_backend(),.memory_cap=8*1024*1024};
    check(os64_text_create(&options,&probe_text)==OS64_FONT_OK,"text context");
    check(os64_text_font_bitmap(probe_text,&probe_font)==OS64_FONT_OK,"bitmap face");
    identity_and_edit();
    property_only();
    script_lifecycle();
    native_refusals();
    switch_cases();
    edit_and_kind_changes();
    password_edits();
    reset_editors();
    noscript_author_styles();
    reclaim_browser_holders();
    snapshot_actions();
    bounded_resources();
    stylesheet_reuse();
    os64_text_font_release(probe_font);
    check(os64_text_destroy(probe_text)==OS64_FONT_OK,"all layout text released");
    check(live==0,"native and engine heap is empty");
    printf("Yonder scripted-page host: %u checks, %u failed\n",checks,failures);
    return failures?1:0;
}
