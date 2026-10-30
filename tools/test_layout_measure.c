#define YONDER_HOST_FAKES_ONLY
#include "test_yonder_scripts_host.c"

static void measure_click(const char *id)
{
    os64_dom_event_t event={.type="click",.bubbles=true,.cancelable=true};
    bool prevented=false;os64_js_outcome_t outcome;
    os64_js_status_t status=yonder_scripts_dispatch(g.page.scripts,probe_id(id),&event,&prevented,&outcome);
    if(status!=OS64_JS_OK) fprintf(stderr,"measurement click: %s\n",outcome.message);
    check(status==OS64_JS_OK,"measurement button executes");
}
static const char *measure_report(void)
{
    os64_html_node_t *node=probe_id("report");
    check(node && node->first_child && node->first_child->kind==OS64_HTML_TEXT,
          "measurement report text reads");
    return probe_text_of("report");
}
static void measure_pump(void)
{
    for(unsigned i=0;i<400;++i) {
        if(script_turn())continue;
        uint64_t next=yonder_scripts_timer_next(g.page.scripts);
        if(next==YONDER_NEVER)return;
        if(next>now_ms){clock_us+=(int64_t)(next-now_ms)*1000;now_ms=next;}
    }
    check(false,"measurement timers finish within bound");
}
int main(int argc,char **argv)
{
    if(argc<2 || argc>3)return 2;
    bool full=argc==3 && !strcmp(argv[2],"--full");
    FILE *f=fopen("tools/pages/layout-measure.html","rb");if(!f)return 2;
    fseek(f,0,SEEK_END);size_t size=ftell(f);rewind(f);char *html=malloc(size+1);
    if(!html||fread(html,1,size,f)!=size)return 2;
    html[size]=0;fclose(f);
    /* Select the fixture's own quick-check choices, leaving its script intact. */
    char *selected;
    while(!full && (selected=strstr(html," selected"))!=NULL)memmove(selected,selected+9,strlen(selected+9)+1);
    os64_text_options_t options={.memory={.alloc=probe_alloc,.free=probe_free},
        .backend=flow_test_backend(),.memory_cap=32*1024*1024};
    check(os64_text_create(&options,&probe_text)==OS64_FONT_OK,"measurement text context");
    check(os64_text_font_bitmap(probe_text,&probe_font)==OS64_FONT_OK,"measurement bitmap font");
    probe_page(html,true);free(html);
    g.page.diag=diag_new_claimed("https://fixture.test/layout-measure",1);s_layout_profile=true;
    measure_pump();measure_click("run");measure_pump();
    const char *report=measure_report();
    if(!strstr(report,"\"state\": \"complete\""))fprintf(stderr,"report: %s\nstatus: %s\n",report,probe_text_of("status"));
    check(strstr(report,"\"state\": \"complete\"") && strstr(report,full ? "\"rows\": 2000" : "\"rows\": 200,") &&
          strstr(report,full ? "\"samples\": 20" : "\"samples\": 3") && strstr(report,"Read unchanged geometry") &&
          strstr(report,"Change one paragraph") && strstr(report,"Replace one list item"),
          "measurement fixture completes all three experiments");
    FILE *out=fopen(argv[1],"w");if(out){fputs(report,out);fclose(out);}else check(false,"measurement report written");
    check(yonder_diag_fact_value(g.page.diag,"layout")!=NULL,"measurement phase totals recorded");
    measure_click("run");measure_click("stop");measure_pump();
    check(strstr(measure_report(),"\"state\": \"cancelled\"")!=NULL,"measurement cancellation invalidates old timer");
    measure_click("run");g.view.bounds.w-=40;measure_pump();
    check(strstr(measure_report(),"\"state\": \"invalid\"")!=NULL,"measurement rejects changed viewport");
    s_layout_profile=false;probe_drop();os64_text_font_release(probe_font);
    check(os64_text_destroy(probe_text)==OS64_FONT_OK,"measurement text released");
    printf("Layout measurement fixture: %u checks, %u failed\n",checks,failures);
    return failures?1:0;
}
