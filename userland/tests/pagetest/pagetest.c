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
    require(os64_page_reset(page,0)==0,"reset");
    require(os64_streq(os64_page_control(page,0)->value,"A") && os64_page_control(page,2)->checked,"normalized reset");
    os64_page_free(page);os64_html_document_free(doc);
}

int main(void)
{
    require(os64_heap_verify()==0,"heap before");
    numeric();state();
    require(os64_heap_verify()==0,"heap after");
    os64_printf("pagetest: numeric conversion, range grids, control state and submission passed\n");
    os64_serial_log("pagetest: PASS numeric conversion, range grids, control state and submission");
    return PAGETEST_OK;
}
