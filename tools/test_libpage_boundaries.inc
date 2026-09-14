static void boundary_unicode(os64_page_t *page)
{
    state_expect("boundary: Unicode edit",os64_page_set_text(page,0,"\xc3\xa9",2) == 0);
}
static void boundary_astral(os64_page_t *page)
{
    state_expect("boundary: astral edit",os64_page_set_text(page,0,"\xf0\x9f\x98\x80",4) == 0);
}
static void boundary_number(os64_page_t *page)
{
    state_expect("boundary: number edit",os64_page_set_text(page,0,"123",3) == 0);
}

static void boundary_failures(const char *html, int index)
{
    os64_html_document_t *doc=parse(html,"utf-8");
    os64_page_t *page=os64_page_build(doc,kPage,NULL);
    os64_page_what_t what={OS64_PAGE_ACTIVATE_CONTROL,index,7,9};
    os64_page_request_t req;
    size_t before=allocations;
    state_expect("boundary: baseline activation",os64_page_activate(page,what,&req)==OS64_PAGE_NAVIGATE);
    size_t count=allocations-before;
    os64_page_request_free(&req);
    fail_single=true;
    for (size_t i=1;i<=count;i++) {
        fail_at=allocations+i;
        os64_page_verdict_t verdict=os64_page_activate(page,what,&req);
        fail_at=0;
        state_expect("boundary: request cannot silently lose failed allocation",
                     verdict==OS64_PAGE_REFUSED && req.reason==OS64_PAGE_REASON_NO_MEMORY);
        os64_page_request_free(&req);
        state_expect("boundary: activation failure leaves model intact",!os64_page_incomplete(page));
    }
    fail_single=false;
    os64_page_free(page);os64_html_document_free(doc);
}

static void boundary_cases(void)
{
    run((PCase){.steps="I7",.name="boundary: maxlength counts UTF16",.html=
        "<form><input name=q maxlength=1><button>Send</button></form>",.index=1,
        .setup=boundary_unicode,.url="http://host/dir/page.html?q=%C3%A9"});
    run((PCase){.steps="I7",.name="boundary: astral needs two UTF16 units",.html=
        "<form><input name=q maxlength=1><button>Send</button></form>",.index=1,
        .setup=boundary_astral,.verdict=OS64_PAGE_NOTHING,.check_reason=true,.reason=OS64_PAGE_REASON_INVALID});
    run((PCase){.steps="I7",.name="boundary: astral fits two UTF16 units",.html=
        "<form><input name=q maxlength=' +2tail'><button>Send</button></form>",.index=1,
        .setup=boundary_astral,.url="http://host/dir/page.html?q=%F0%9F%98%80"});
    run((PCase){.steps="I7",.name="boundary: length limits do not apply to numbers",.html=
        "<form><input type=number name=q maxlength=1><button>Send</button></form>",.index=1,
        .setup=boundary_number,.url="http://host/dir/page.html?q=123"});
    run((PCase){.steps="F8",.name="boundary: data GET ignores plain enctype",.html=
        "<form action='data:text/plain,x' enctype=text/plain><input name=q value='a b'>"
        "<button>Send</button></form>",.index=1,.url="data:text/plain,x?q=a+b"});
    run((PCase){.steps="F12",.name="boundary: mail GET ignores plain enctype",.html=
        "<form action='mailto:a@b' enctype=text/plain><input name=q value='a b'>"
        "<button>Send</button></form>",.index=1,.url="mailto:a@b?q=a%20b"});
    run((PCase){.steps="F13",.name="boundary: mail POST plain body is escaped UTF8",.html=
        "<form action='mailto:a@b?subject=hello' method=post enctype=text/plain accept-charset=windows-1252>"
        "<input name=q value='\xc3\xa9 #?'><button>Send</button></form>",.index=1,
        .url="mailto:a@b?subject=hello&body=q=%C3%A9%20%23%3F%0D%0A"});
    run((PCase){.steps="A4",.name="boundary: URL whitespace before split",.html=
        "<base href=' https://ho&#9;st/dir/ '><a href=' ../next#part '>go</a>",.link=true,
        .url="https://host/next",.fragment="part"});
    run((PCase){.steps="A4,G3",.name="boundary: fragment resolves against base",.html=
        "<base href=https://elsewhere/path><a href=#part>go</a>",.link=true,
        .url="https://elsewhere/path",.fragment="part"});
    run((PCase){.steps="A5",.name="boundary: empty link resolves against base",.html=
        "<base href=https://elsewhere/path><a href=''>go</a>",.link=true,.url="https://elsewhere/path"});
    run((PCase){.steps="A5",.name="boundary: empty action still uses document",.html=
        "<base href=https://elsewhere/path><form action=''><input name=q value=1><button>Go</button></form>",
        .index=1,.url="http://host/dir/page.html?q=1"});
    run((PCase){.steps="D8",.name="boundary: hard wrap is a submission value",.html=
        "<form method=post><textarea name=q wrap=hard cols=3>abcdefghij</textarea><button>Go</button></form>",
        .index=1,.body="q=abc%0D%0Adef%0D%0Aghi%0D%0Aj"});
    run((PCase){.steps="D8",.name="boundary: hard wrap preserves Unicode scalars and existing LF",.html=
        "<form method=post><textarea name=q wrap=hard cols=' +1tail'>\xc3\xa9\xf0\x9f\x98\x80\nx</textarea>"
        "<button>Go</button></form>",.index=1,.body="q=%C3%A9%0D%0A%F0%9F%98%80%0D%0Ax"});
    value_case("H7","boundary: local datetime uses shortest normalized time",
        "<input type=datetime-local value='2020-01-01 12:30:00.000'>",0,"2020-01-01T12:30");
    value_case("H7","boundary: local datetime retains nonzero fractional seconds",
        "<input type=datetime-local value='2020-01-01 12:30:01.120'>",0,"2020-01-01T12:30:01.12");
    value_case("H8","boundary: long positive year keeps calendar residue",
        "<input type=date value='00000000000000000000000002000-02-29'>",0,
        "00000000000000000000000002000-02-29");
    value_case("H9","boundary: ISO week 2020 has 53", "<input type=week value=2020-W53>",0,"2020-W53");
    value_case("H9","boundary: ISO week 2021 has 52", "<input type=week value=2021-W53>",0,"");

    os64_html_document_t *doc=parse("<form method=post><input name=q value=1><button>Go</button></form>","utf-8");
    for (size_t cap=2;cap<=4;cap++) {
        os64_page_options_t opt=os64_page_options_default();opt.max_body=cap;
        os64_page_t *page=os64_page_build(doc,kPage,&opt);
        os64_page_request_t req;os64_page_what_t what={OS64_PAGE_ACTIVATE_CONTROL,1,0,0};
        os64_page_verdict_t result=os64_page_activate(page,what,&req);
        state_expect("boundary: exact body ceiling",cap<3 ? result==OS64_PAGE_REFUSED &&
            req.reason==OS64_PAGE_REASON_BODY_TOO_LONG : result==OS64_PAGE_NAVIGATE && req.body_len==3);
        os64_page_request_free(&req);os64_page_free(page);
    }
    os64_html_document_free(doc);
    char html[1200],name[1023],want[2200];
    memset(name,'n',1022);name[1022]='\0';
    snprintf(html,sizeof(html),"<form method=post><input type=image name='%s'></form>",name);
    snprintf(want,sizeof(want),"%s.x=7&%s.y=9",name,name);
    run((PCase){.steps="D3",.name="boundary: image name has room for both suffixes",.html=html,.x=7,.y=9,.body=want});
    boundary_failures(html,0);
    boundary_failures("<form method=post action='/next#part'><select name=q><option>abc</select>"
                      "<button>Go</button></form>",1);
    boundary_failures("<form method=post enctype=text/plain action='mailto:a@b#part'><input name=q value='a b'>"
                      "<button>Go</button></form>",1);
    boundary_failures("<form method=post><textarea name=q wrap=hard cols=1>ab</textarea>"
                      "<button>Go</button></form>",1);
}
