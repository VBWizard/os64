#define YONDER_HOST_FAKES_ONLY
#include "test_yonder_scripts_host.c"
static void pump(void)
{
    for(unsigned i=0;i<300;++i) {
        if(script_turn())continue;
        uint64_t next=yonder_scripts_timer_next(g.page.scripts);
        if(next==YONDER_NEVER)return;
        if(next>now_ms){clock_us+=(int64_t)(next-now_ms)*1000;now_ms=next;}
    }
    check(false,"graft fixture: timer bound");
}
static void click(const char *id)
{
    os64_dom_event_t event={.type="click",.bubbles=true,.cancelable=true};
    bool prevented=false;os64_js_outcome_t outcome;
    check(yonder_scripts_dispatch(g.page.scripts,probe_id(id),&event,&prevented,&outcome)==OS64_JS_OK,
          "graft fixture: button dispatch");
}
int main(int argc,char **argv)
{
    if(argc!=2)return 2;
    FILE *f=fopen("tools/pages/layout-graft.html","rb");if(!f)return 2;
    fseek(f,0,SEEK_END);size_t size=ftell(f);rewind(f);char *html=malloc(size+1);
    if(!html||fread(html,1,size,f)!=size)return 2;
    html[size]=0;fclose(f);
    os64_text_options_t options={.memory={.alloc=probe_alloc,.free=probe_free},
        .backend=flow_test_backend(),.memory_cap=32*1024*1024};
    check(os64_text_create(&options,&probe_text)==OS64_FONT_OK,"graft fixture: text context");
    check(os64_text_font_bitmap(probe_text,&probe_font)==OS64_FONT_OK,"graft fixture: font");
    probe_page(html,true);free(html);
    g.page.diag=diag_new_claimed("https://fixture.test/layout-graft",1);s_layout_profile=true;
    pump();click("change");check(script_rebuild(),"graft fixture: manual paragraph");
    click("append");check(script_rebuild(),"graft fixture: manual append");
    click("run");pump();
    const char *report=probe_text_of("report");
    check(strstr(report,"\"state\": \"complete\"") && strstr(report,"\"rows\": 2000") &&
          strstr(report,"\"samples\": 20") && strstr(report,"Append one list row"),
          "graft fixture: both full experiments finish");
    FILE *out=fopen(argv[1],"w");if(out){fprintf(out,"%s\nlayout: %s\ngraft: %s\n",report,
        yonder_diag_fact_value(g.page.diag,"layout"),yonder_diag_fact_value(g.page.diag,"layout graft"));fclose(out);}
    else check(false,"graft fixture: report saved");
    click("run");click("stop");pump();
    check(strstr(probe_text_of("report"),"\"state\": \"cancelled\"")!=NULL,"graft fixture: cancellation");
    click("run");g.view.bounds.w-=20;pump();
    check(strstr(probe_text_of("report"),"\"state\": \"invalid\"")!=NULL,"graft fixture: changed viewport");
    s_layout_profile=false;probe_drop();os64_text_font_release(probe_font);
    check(os64_text_destroy(probe_text)==OS64_FONT_OK && live==0,"graft fixture: ownership released");
    printf("Layout graft fixture: %u checks, %u failed\n",checks,failures);return failures?1:0;
}
