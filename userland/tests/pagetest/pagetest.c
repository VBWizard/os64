#include "html/html.h"
#include "page/page.h"
#include "os64/os64.h"

#define PAGETEST_OK 0x50670000u
#define PAGETEST_FAIL 0x50670001u

static void require(bool condition, const char *why)
{
    if (condition) return;
    os64_printf("pagetest: FAIL %s\n",why);
    os64_serial_log(why);
    os64_exit(PAGETEST_FAIL);
}

static os64_page_t *build(const char *html, os64_html_document_t **doc)
{
    os64_html_parser_t *parser=os64_html_parser_new(NULL);
    require(parser != NULL,"parser allocation");
    require(os64_html_parser_feed(parser,html,os64_strlen(html)) == 0,"parse feed");
    *doc=os64_html_parser_finish(parser);
    require(*doc != NULL && !(*doc)->refusal,"parse finish");
    os64_page_t *page=os64_page_build(*doc,"https://host/page",NULL);
    require(page != NULL && !os64_page_incomplete(page),"complete page model");
    return page;
}

static void numeric(void)
{
    static const struct {const char *html,*value;} cases[]={
        {"<input type=range min=1 max=10>","6"},
        {"<input type=range min=0 max=1 step=0.1 value=0.25>","0.3"},
        {"<input type=range min=0 max=10 value=100>","10"},
        {"<input type=range min=1e-20 max=1 step=any>","0.5"},
        {"<input type=range min=-1e308 max=1e308 step=any>","0"},
        {"<input type=range min=1e-300 max=1 step=7 value=0.25>","1e-300"},
        {"<input type=number value=12345678901>","12345678901"},
        {"<input type=number value=.5>",".5"},
        {"<input type=number value=1e999>",""},
        {"<input type=number value=5e-324>","5e-324"},
        {"<input type=week value=2026-W53>","2026-W53"},
        {"<input type=week value=2021-W53>",""},
        {"<input type=datetime-local value='2020-01-01 12:30:00.000'>","2020-01-01T12:30"}
    };
    for (unsigned round=0;round<8;round++)
        for (size_t i=0;i<sizeof(cases)/sizeof(cases[0]);i++) {
            os64_html_document_t *doc;
            os64_page_t *page=build(cases[i].html,&doc);
            require(os64_streq(os64_page_control(page,0)->value,cases[i].value),cases[i].html);
            os64_page_free(page);os64_html_document_free(doc);
            os64_yield();
        }
}

static void state(void)
{
    os64_html_document_t *doc;
    os64_page_t *page=build("<form method=post action=/send><select name=s><option label=Short>A<option>B</select>"
        "<input type=radio name=r value=a checked><input type=radio name=r value=b checked>"
        "<textarea name=t wrap=hard cols=3>abcdef</textarea><button>Go</button></form>",&doc);
    require(!os64_page_control(page,1)->checked && os64_page_control(page,2)->checked,"radio default group");
    require(os64_page_set_chosen(page,0,1,true)==0,"select edit");
    require(os64_page_set_checked(page,1,true)==0,"radio edit");
    os64_page_request_t req;
    os64_page_what_t what={OS64_PAGE_ACTIVATE_CONTROL,4,0,0};
    require(os64_page_activate(page,what,&req)==OS64_PAGE_NAVIGATE,"submission");
    require(req.method==OS64_PAGE_METHOD_POST && os64_streq(req.url,"https://host/send"),"submission target");
    require(os64_streq((const char *)req.body,"s=B&r=a&t=abc%0D%0Adef"),"serialized state");
    os64_page_request_free(&req);
    require(os64_page_set_text(page,3,"a\0b\r\nc",6)==0,"embedded NUL edit");
    const os64_page_control_t *text=os64_page_control(page,3);
    require(text->value_len==5 && os64_memcmp(text->value,"a\0b\nc",5)==0,"embedded NUL value span");
    require(os64_page_activate(page,what,&req)==OS64_PAGE_NAVIGATE,"embedded NUL submission");
    require(os64_streq((const char *)req.body,"s=B&r=a&t=a%00b%0D%0Ac"),"embedded NUL request bytes");
    os64_page_request_free(&req);
    require(os64_page_reset(page,0)==0,"reset");
    require(os64_streq(os64_page_control(page,0)->value,"A") && os64_page_control(page,2)->checked,"normalized reset");
    os64_page_free(page);os64_html_document_free(doc);
}

static void navigation(void)
{
    os64_html_document_t *doc;
    os64_page_t *page=build("<form method=post><div dir=auto><bdi>&#1488;</bdi>A"
        "<input name=q dirname=d></div><button>Go</button></form>"
        "<button type=button readonly>B</button><input type=checkbox readonly>"
        "<a name=x href=/else>Legacy</a><h2 id=x>Winner</h2>",&doc);
    require(!os64_page_control(page,1)->barred_from_validation,"submit validation candidate");
    require(os64_page_control(page,2)->barred_from_validation,"inert button validation bar");
    require(!os64_page_control(page,2)->readonly && !os64_page_control(page,3)->readonly,"effective readonly");
    os64_page_request_t req;
    require(os64_page_activate(page,(os64_page_what_t){OS64_PAGE_ACTIVATE_CONTROL,1,0,0},&req)==OS64_PAGE_NAVIGATE,"direction submission");
    require(os64_streq(req.body,"q=&d=ltr"),"isolated text direction");
    os64_page_request_free(&req);
    const os64_html_node_t *node=NULL;
    require(os64_page_resolve_fragment(page,"x",&node)==0 && node && node->tag==OS64_HTML_TAG_H2,"ID precedence");
    require(os64_page_resolve_fragment(page,"top",&node)==0 && node==NULL,"top fallback");
    os64_page_free(page);os64_html_document_free(doc);
    page=build("<form><input name=q value=abcdef><button>Go</button>",&doc);
    os64_page_free(page);
    os64_page_options_t opt=os64_page_options_default();opt.max_body=1;
    page=os64_page_build(doc,"https://host/page",&opt);
    require(page && os64_page_activate(page,(os64_page_what_t){OS64_PAGE_ACTIVATE_CONTROL,1,0,0},&req)==OS64_PAGE_NAVIGATE,"GET independent of body limit");
    require(req.body==NULL && os64_streq(req.url,"https://host/page?q=abcdef"),"GET query bytes");
    os64_page_request_free(&req);os64_page_free(page);os64_html_document_free(doc);
}

static void reference_edges(void)
{
    os64_html_document_t *doc;
    os64_page_t *page=build("<p id=foo>Prefix</p><a href='#foo%00bar'>Go</a>"
        "<meta http-equiv=refresh content='0;url=#%00'>",&doc);
    os64_page_request_t req;
    require(os64_page_activate(page,(os64_page_what_t){OS64_PAGE_ACTIVATE_LINK,0,0,0},&req)==OS64_PAGE_REFUSED &&
        req.reason==OS64_PAGE_REASON_BAD_ACTION,"NUL fragment refused");
    os64_page_request_free(&req);
    require(os64_page_refresh(page)==NULL,"NUL refresh ignored");
    os64_page_free(page);os64_html_document_free(doc);
    page=build("<p id=target>Target</p><a href='data:text/plain,MiXeD#target'>Go</a>",&doc);
    os64_page_free(page);
    page=os64_page_build(doc,"DATA:text/plain,MiXeD",NULL);
    require(page && os64_page_activate(page,(os64_page_what_t){OS64_PAGE_ACTIVATE_LINK,0,0,0},&req)==OS64_PAGE_FRAGMENT &&
        req.anchor!=NULL,"opaque scheme same-document fragment");
    os64_page_request_free(&req);os64_page_free(page);os64_html_document_free(doc);
}

int main(void)
{
    require(os64_heap_verify()==0,"heap before");
    numeric();state();navigation();reference_edges();
    require(os64_heap_verify()==0,"heap after");
    os64_printf("pagetest: numeric conversion, range grids, control state and submission passed\n");
    os64_serial_log("pagetest: PASS numeric conversion, range grids, control state and submission");
    return PAGETEST_OK;
}
